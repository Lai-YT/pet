/*
 * Copyright 2011 Leiden University. All rights reserved.
 * Copyright 2012 Ecole Normale Superieure. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY LEIDEN UNIVERSITY ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LEIDEN UNIVERSITY OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Leiden University.
 */ 

#include <stdlib.h>
#include <map>
#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/Host.h>
#include <clang/Basic/Version.h>
#include <clang/Basic/FileSystemOptions.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/TargetOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/DiagnosticOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Frontend/HeaderSearchOptions.h>
#include <clang/Frontend/LangStandard.h>
#include <clang/Frontend/PreprocessorOptions.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/Pragma.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/Sema/Sema.h>
#include <clang/Sema/SemaDiagnostic.h>
#include <clang/Parse/Parser.h>
#include <clang/Parse/ParseAST.h>

#include <isl/ctx.h>
#include <isl/constraint.h>

#include "options.h"
#include "scan.h"

#include "config.h"

#define ARRAY_SIZE(array) (sizeof(array)/sizeof(*array))

using namespace std;
using namespace clang;
using namespace clang::driver;

/* Called if we found something we didn't expect in one of the pragmas.
 * We'll provide more informative warnings later.
 */
static void unsupported(Preprocessor &PP, SourceLocation loc)
{
	DiagnosticsEngine &diag = PP.getDiagnostics();
	unsigned id = diag.getCustomDiagID(DiagnosticsEngine::Warning,
					   "unsupported");
	DiagnosticBuilder B = diag.Report(loc, id);
}

static int get_int(const char *s)
{
	return s[0] == '"' ? atoi(s + 1) : atoi(s);
}

static ValueDecl *get_value_decl(Sema &sema, Token &token)
{
	IdentifierInfo *name;
	Decl *decl;

	if (token.isNot(tok::identifier))
		return NULL;

	name = token.getIdentifierInfo();
	decl = sema.LookupSingleName(sema.TUScope, name,
				token.getLocation(), Sema::LookupOrdinaryName);
	return decl ? cast_or_null<ValueDecl>(decl) : NULL;
}

/* Handle pragmas of the form
 *
 *	#pragma value_bounds identifier lower_bound upper_bound
 *
 * For each such pragma, add a mapping
 *	{ identifier[] -> [i] : lower_bound <= i <= upper_bound }
 * to value_bounds.
 */
struct PragmaValueBoundsHandler : public PragmaHandler {
	Sema &sema;
	isl_ctx *ctx;
	isl_union_map *value_bounds;

	PragmaValueBoundsHandler(isl_ctx *ctx, Sema &sema) :
	    PragmaHandler("value_bounds"), ctx(ctx), sema(sema) {
		isl_space *space = isl_space_params_alloc(ctx, 0);
		value_bounds = isl_union_map_empty(space);
	}

	~PragmaValueBoundsHandler() {
		isl_union_map_free(value_bounds);
	}

	virtual void HandlePragma(Preprocessor &PP,
				  PragmaIntroducerKind Introducer,
				  Token &ScopTok) {
		isl_id *id;
		isl_space *dim;
		isl_map *map;
		ValueDecl *vd;
		Token token;
		int lb;
		int ub;

		PP.Lex(token);
		vd = get_value_decl(sema, token);
		if (!vd) {
			unsupported(PP, token.getLocation());
			return;
		}

		PP.Lex(token);
		if (!token.isLiteral()) {
			unsupported(PP, token.getLocation());
			return;
		}

		lb = get_int(token.getLiteralData());

		PP.Lex(token);
		if (!token.isLiteral()) {
			unsupported(PP, token.getLocation());
			return;
		}

		ub = get_int(token.getLiteralData());

		dim = isl_space_alloc(ctx, 0, 0, 1);
		map = isl_map_universe(dim);
		map = isl_map_lower_bound_si(map, isl_dim_out, 0, lb);
		map = isl_map_upper_bound_si(map, isl_dim_out, 0, ub);
		id = isl_id_alloc(ctx, vd->getName().str().c_str(), vd);
		map = isl_map_set_tuple_id(map, isl_dim_in, id);

		value_bounds = isl_union_map_add_map(value_bounds, map);
	}
};

/* Given a variable declaration, check if it has an integer initializer
 * and if so, add a parameter corresponding to the variable to "value"
 * with its value fixed to the integer initializer and return the result.
 */
static __isl_give isl_set *extract_initialization(__isl_take isl_set *value,
	ValueDecl *decl)
{
	VarDecl *vd;
	Expr *expr;
	IntegerLiteral *il;
	isl_int v;
	isl_ctx *ctx;
	isl_id *id;
	isl_space *space;
	isl_set *set;

	vd = cast<VarDecl>(decl);
	if (!vd)
		return value;
	if (!vd->getType()->isIntegerType())
		return value;
	expr = vd->getInit();
	if (!expr)
		return value;
	il = cast<IntegerLiteral>(expr);
	if (!il)
		return value;

	ctx = isl_set_get_ctx(value);
	id = isl_id_alloc(ctx, vd->getName().str().c_str(), vd);
	space = isl_space_params_alloc(ctx, 1);
	space = isl_space_set_dim_id(space, isl_dim_param, 0, id);
	set = isl_set_universe(space);

	isl_int_init(v);
	PetScan::extract_int(il, &v);
	set = isl_set_fix(set, isl_dim_param, 0, v);
	isl_int_clear(v);

	return isl_set_intersect(value, set);
}

/* Handle pragmas of the form
 *
 *	#pragma parameter identifier lower_bound
 * and
 *	#pragma parameter identifier lower_bound upper_bound
 *
 * For each such pragma, intersect the context with the set
 * [identifier] -> { [] : lower_bound <= identifier <= upper_bound }
 */
struct PragmaParameterHandler : public PragmaHandler {
	Sema &sema;
	isl_set *&context;
	isl_set *&context_value;

	PragmaParameterHandler(Sema &sema, isl_set *&context,
		isl_set *&context_value) :
		PragmaHandler("parameter"), sema(sema), context(context),
		context_value(context_value) {}

	virtual void HandlePragma(Preprocessor &PP,
				  PragmaIntroducerKind Introducer,
				  Token &ScopTok) {
		isl_id *id;
		isl_ctx *ctx = isl_set_get_ctx(context);
		isl_space *dim;
		isl_set *set;
		ValueDecl *vd;
		Token token;
		int lb;
		int ub;
		bool has_ub = false;

		PP.Lex(token);
		vd = get_value_decl(sema, token);
		if (!vd) {
			unsupported(PP, token.getLocation());
			return;
		}

		PP.Lex(token);
		if (!token.isLiteral()) {
			unsupported(PP, token.getLocation());
			return;
		}

		lb = get_int(token.getLiteralData());

		PP.Lex(token);
		if (token.isLiteral()) {
			has_ub = true;
			ub = get_int(token.getLiteralData());
		} else if (token.isNot(tok::eod)) {
			unsupported(PP, token.getLocation());
			return;
		}

		id = isl_id_alloc(ctx, vd->getName().str().c_str(), vd);
		dim = isl_space_params_alloc(ctx, 1);
		dim = isl_space_set_dim_id(dim, isl_dim_param, 0, id);

		set = isl_set_universe(dim);

		set = isl_set_lower_bound_si(set, isl_dim_param, 0, lb);
		if (has_ub)
			set = isl_set_upper_bound_si(set, isl_dim_param, 0, ub);

		context = isl_set_intersect(context, set);

		context_value = extract_initialization(context_value, vd);
	}
};

/* Handle pragmas of the form
 *
 *	#pragma scop
 *
 * In particular, store the current location in loc.start.
 */
struct PragmaScopHandler : public PragmaHandler {
	ScopLoc &loc;

	PragmaScopHandler(ScopLoc &loc) : PragmaHandler("scop"), loc(loc) {}

	virtual void HandlePragma(Preprocessor &PP,
				  PragmaIntroducerKind Introducer,
				  Token &ScopTok) {
		SourceManager &SM = PP.getSourceManager();
		loc.start = SM.getFileOffset(ScopTok.getLocation());
	}
};

/* Handle pragmas of the form
 *
 *	#pragma endscop
 *
 * In particular, store the current location in loc.end.
 */
struct PragmaEndScopHandler : public PragmaHandler {
	ScopLoc &loc;

	PragmaEndScopHandler(ScopLoc &loc) :
		PragmaHandler("endscop"), loc(loc) {}

	virtual void HandlePragma(Preprocessor &PP,
				  PragmaIntroducerKind Introducer,
				  Token &EndScopTok) {
		SourceManager &SM = PP.getSourceManager();
		loc.end = SM.getFileOffset(EndScopTok.getLocation());
	}
};

/* Handle pragmas of the form
 *
 *	#pragma live-out identifier, identifier, ...
 *
 * Each identifier on the line is stored in live_out.
 */
struct PragmaLiveOutHandler : public PragmaHandler {
	Sema &sema;
	set<ValueDecl *> &live_out;

	PragmaLiveOutHandler(Sema &sema, set<ValueDecl *> &live_out) :
		PragmaHandler("live"), sema(sema), live_out(live_out) {}

	virtual void HandlePragma(Preprocessor &PP,
				  PragmaIntroducerKind Introducer,
				  Token &ScopTok) {
		Token token;

		PP.Lex(token);
		if (token.isNot(tok::minus))
			return;
		PP.Lex(token);
		if (token.isNot(tok::identifier) ||
		    !token.getIdentifierInfo()->isStr("out"))
			return;

		PP.Lex(token);
		while (token.isNot(tok::eod)) {
			ValueDecl *vd;

			vd = get_value_decl(sema, token);
			if (!vd) {
				unsupported(PP, token.getLocation());
				return;
			}
			live_out.insert(vd);
			PP.Lex(token);
			if (token.is(tok::comma))
				PP.Lex(token);
		}
	}
};

/* Extract a pet_scop from the appropriate function.
 * If "function" is not NULL, then we only extract a pet_scop if the
 * name of the function matches.
 * If "autodetect" is false, then we only extract if we have seen
 * scop and endscop pragmas and if these are situated inside the function
 * body.
 */
struct PetASTConsumer : public ASTConsumer {
	Preprocessor &PP;
	ASTContext &ast_context;
	ScopLoc &loc;
	const char *function;
	pet_options *options;
	isl_ctx *ctx;
	struct pet_scop *scop;
	PragmaValueBoundsHandler *vb_handler;

	PetASTConsumer(isl_ctx *ctx, Preprocessor &PP, ASTContext &ast_context,
		ScopLoc &loc, const char *function, pet_options *options) :
		ctx(ctx), PP(PP), ast_context(ast_context), loc(loc),
		scop(NULL), function(function), options(options),
		vb_handler(NULL) { }

	void handle_value_bounds(Sema *sema) {
		vb_handler = new PragmaValueBoundsHandler(ctx, *sema);
		PP.AddPragmaHandler(vb_handler);
	}

	__isl_give isl_union_map *get_value_bounds() {
		return isl_union_map_copy(vb_handler->value_bounds);
	}

	virtual HandleTopLevelDeclReturn HandleTopLevelDecl(DeclGroupRef dg) {
		DeclGroupRef::iterator it;

		if (scop)
			return HandleTopLevelDeclContinue;
		for (it = dg.begin(); it != dg.end(); ++it) {
			isl_union_map *vb = vb_handler->value_bounds;
			FunctionDecl *fd = dyn_cast<clang::FunctionDecl>(*it);
			if (!fd)
				continue;
			if (!fd->hasBody())
				continue;
			if (function &&
			    fd->getNameInfo().getAsString() != function)
				continue;
			if (options->autodetect) {
				PetScan ps(PP, ast_context, loc, options,
					    isl_union_map_copy(vb));
				scop = ps.scan(fd);
				if (scop)
					break;
				else
					continue;
			}
			if (!loc.end)
				continue;
			SourceManager &SM = PP.getSourceManager();
			if (SM.getFileOffset(fd->getLocStart()) > loc.end)
				continue;
			if (SM.getFileOffset(fd->getLocEnd()) < loc.start)
				continue;
			PetScan ps(PP, ast_context, loc, options,
				    isl_union_map_copy(vb));
			scop = ps.scan(fd);
			break;
		}

		return HandleTopLevelDeclContinue;
	}
};

static const char *ResourceDir = CLANG_PREFIX"/lib/clang/"CLANG_VERSION_STRING;

static const char *implicit_functions[] = {
	"min", "max", "ceild", "floord"
};

static bool is_implicit(const IdentifierInfo *ident)
{
	const char *name = ident->getNameStart();
	for (int i = 0; i < ARRAY_SIZE(implicit_functions); ++i)
		if (!strcmp(name, implicit_functions[i]))
			return true;
	return false;
}

/* Ignore implicit function declaration warnings on
 * "min", "max", "ceild" and "floord" as we detect and handle these
 * in PetScan.
 *
 * The cloned field keeps track of whether the clone method
 * has ever been called.  Newer clangs (by default) clone
 * the DiagnosticConsumer passed to createDiagnostics and
 * then take ownership of the clone, which means that
 * the original has to be deleted by the calling code.
 */
struct MyDiagnosticPrinter : public TextDiagnosticPrinter {
	const DiagnosticOptions *DiagOpts;
	static bool cloned;
	MyDiagnosticPrinter(const DiagnosticOptions &DO) :
		DiagOpts(&DO), TextDiagnosticPrinter(llvm::errs(), DO) {}
	virtual DiagnosticConsumer *clone(DiagnosticsEngine &Diags) const {
		cloned = true;
		return new MyDiagnosticPrinter(*DiagOpts);
	}
	virtual void HandleDiagnostic(DiagnosticsEngine::Level level,
					const DiagnosticInfo &info) {
		if (info.getID() == diag::ext_implicit_function_decl &&
		    info.getNumArgs() == 1 &&
		    info.getArgKind(0) == DiagnosticsEngine::ak_identifierinfo &&
		    is_implicit(info.getArgIdentifier(0)))
			/* ignore warning */;
		else
			TextDiagnosticPrinter::HandleDiagnostic(level, info);
	}
};

bool MyDiagnosticPrinter::cloned = false;

/* For each array in "scop", set its value_bounds property
 * based on the infofrmation in "value_bounds" and
 * mark it as live_out if it appears in "live_out".
 */
static void update_arrays(struct pet_scop *scop,
	__isl_take isl_union_map *value_bounds, set<ValueDecl *> &live_out)
{
	set<ValueDecl *>::iterator lo_it;
	isl_ctx *ctx = isl_union_map_get_ctx(value_bounds);

	if (!scop) {
		isl_union_map_free(value_bounds);
		return;
	}

	for (int i = 0; i < scop->n_array; ++i) {
		isl_id *id;
		isl_space *space;
		isl_map *bounds;
		ValueDecl *decl;
		pet_array *array = scop->arrays[i];

		id = isl_set_get_tuple_id(array->extent);
		decl = (ValueDecl *)isl_id_get_user(id);

		space = isl_space_alloc(ctx, 0, 0, 1);
		space = isl_space_set_tuple_id(space, isl_dim_in, id);

		bounds = isl_union_map_extract_map(value_bounds, space);
		if (!isl_map_plain_is_empty(bounds))
			array->value_bounds = isl_map_range(bounds);
		else
			isl_map_free(bounds);

		lo_it = live_out.find(decl);
		if (lo_it != live_out.end())
			array->live_out = 1;
	}

	isl_union_map_free(value_bounds);
}

#ifdef USE_ARRAYREF

#ifdef HAVE_CXXISPRODUCTION
static Driver *construct_driver(const char *binary, DiagnosticsEngine &Diags)
{
	return new Driver(binary, llvm::sys::getDefaultTargetTriple(),
			    "", false, false, Diags);
}
#else
static Driver *construct_driver(const char *binary, DiagnosticsEngine &Diags)
{
	return new Driver(binary, llvm::sys::getDefaultTargetTriple(),
			    "", false, Diags);
}
#endif

/* Create a CompilerInvocation object that stores the command line
 * arguments constructed by the driver.
 * The arguments are mainly useful for setting up the system include
 * paths on newer clangs and on some platforms.
 */
static CompilerInvocation *construct_invocation(const char *filename,
	DiagnosticsEngine &Diags)
{
	const char *binary = CLANG_PREFIX"/bin/clang";
	const llvm::OwningPtr<Driver> driver(construct_driver(binary, Diags));
	std::vector<const char *> Argv;
	Argv.push_back(binary);
	Argv.push_back(filename);
	const llvm::OwningPtr<Compilation> compilation(
		driver->BuildCompilation(llvm::ArrayRef<const char *>(Argv)));
	JobList &Jobs = compilation->getJobs();
	if (Jobs.size() < 1)
		return NULL;

	Command *cmd = cast<Command>(*Jobs.begin());
	if (strcmp(cmd->getCreator().getName(), "clang"))
		return NULL;

	const ArgStringList *args = &cmd->getArguments();

	CompilerInvocation *invocation = new CompilerInvocation;
	CompilerInvocation::CreateFromArgs(*invocation, args->data() + 1,
						args->data() + args->size(),
						Diags);
	return invocation;
}

#else

static CompilerInvocation *construct_invocation(const char *filename,
	DiagnosticsEngine &Diags)
{
	return NULL;
}

#endif

/* Extract a pet_scop from the C source file called "filename".
 * If "function" is not NULL, extract the pet_scop from the function
 * with that name.
 * If "autodetect" is set, extract any pet_scop we can find.
 * Otherwise, extract the pet_scop from the region delimited
 * by "scop" and "endscop" pragmas.
 *
 * We first set up the clang parser and then try to extract the
 * pet_scop from the appropriate function in PetASTConsumer.
 * If we have found a pet_scop, we add the context and value_bounds
 * constraints specified through pragmas.
 */
static struct pet_scop *scop_extract_from_C_source(isl_ctx *ctx,
	const char *filename, const char *function, pet_options *options)
{
	isl_space *dim;
	isl_set *context;
	isl_set *context_value;
	pet_scop *scop;
	set<ValueDecl *> live_out;
	isl_union_map *value_bounds;

	CompilerInstance *Clang = new CompilerInstance();
	DiagnosticOptions DO;
	MyDiagnosticPrinter *printer = new MyDiagnosticPrinter(DO);
	Clang->createDiagnostics(0, NULL, printer);
	if (printer->cloned)
		delete printer;
	DiagnosticsEngine &Diags = Clang->getDiagnostics();
	Diags.setSuppressSystemWarnings(true);
	CompilerInvocation *invocation = construct_invocation(filename, Diags);
	if (invocation)
		Clang->setInvocation(invocation);
	Clang->createFileManager();
	Clang->createSourceManager(Clang->getFileManager());
	TargetOptions TO;
	TO.Triple = llvm::sys::getDefaultTargetTriple();
	TargetInfo *target = TargetInfo::CreateTargetInfo(Diags, TO);
	Clang->setTarget(target);
	CompilerInvocation::setLangDefaults(Clang->getLangOpts(), IK_C,
					    LangStandard::lang_unspecified);
	HeaderSearchOptions &HSO = Clang->getHeaderSearchOpts();
	HSO.ResourceDir = ResourceDir;
	for (int i = 0; i < options->n_path; ++i)
		HSO.AddPath(options->paths[i],
			frontend::Angled, true, false, false);
	PreprocessorOptions &PO = Clang->getPreprocessorOpts();
	for (int i = 0; i < options->n_define; ++i)
		PO.addMacroDef(options->defines[i]);
	Clang->createPreprocessor();
	Preprocessor &PP = Clang->getPreprocessor();

	ScopLoc loc;

	const FileEntry *file = Clang->getFileManager().getFile(filename);
	if (!file)
		isl_die(ctx, isl_error_unknown, "unable to open file",
			do { delete Clang; return NULL; } while (0));
	Clang->getSourceManager().createMainFileID(file);

	Clang->createASTContext();
	PetASTConsumer consumer(ctx, PP, Clang->getASTContext(),
				loc, function, options);
	Sema *sema = new Sema(PP, Clang->getASTContext(), consumer);

	if (!options->autodetect) {
		PP.AddPragmaHandler(new PragmaScopHandler(loc));
		PP.AddPragmaHandler(new PragmaEndScopHandler(loc));
		PP.AddPragmaHandler(new PragmaLiveOutHandler(*sema, live_out));
	}

	dim = isl_space_params_alloc(ctx, 0);
	context = isl_set_universe(isl_space_copy(dim));
	context_value = isl_set_universe(dim);
	PP.AddPragmaHandler(new PragmaParameterHandler(*sema, context,
							context_value));
	consumer.handle_value_bounds(sema);

	Diags.getClient()->BeginSourceFile(Clang->getLangOpts(), &PP);
	ParseAST(*sema);
	Diags.getClient()->EndSourceFile();

	scop = consumer.scop;
	if (Diags.hasErrorOccurred()) {
		pet_scop_free(scop);
		scop = NULL;
	}

	if (scop) {
		scop->context = isl_set_intersect(context, scop->context);
		scop->context_value = isl_set_intersect(context_value,
						scop->context_value);
	} else {
		isl_set_free(context);
		isl_set_free(context_value);
	}

	scop = pet_scop_anonymize(scop);

	update_arrays(scop, consumer.get_value_bounds(), live_out);

	delete sema;
	delete Clang;

	return scop;
}

struct pet_scop *pet_scop_extract_from_C_source(isl_ctx *ctx,
	const char *filename, const char *function)
{
	pet_scop *scop;
	pet_options *options;
	bool allocated = false;

	options = isl_ctx_peek_pet_options(ctx);
	if (!options) {
		options = pet_options_new_with_defaults();
		allocated = true;
	}

	scop = scop_extract_from_C_source(ctx, filename, function, options);
	llvm::llvm_shutdown();

	if (allocated)
		pet_options_free(options);

	return scop;
}
