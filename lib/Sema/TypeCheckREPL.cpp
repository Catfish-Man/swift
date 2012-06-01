//===--- TypeCheckREPL.cpp - Type Checking for the REPL -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements REPL-specific semantic analysis rules.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Expr.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Stmt.h"

using namespace swift;

void
PrintLiteralString(StringRef Str, ASTContext &Context, SourceLoc Loc,
                   SmallVectorImpl<ValueDecl*> &PrintDecls,
                   SmallVectorImpl<BraceStmt::ExprStmtOrDecl> &BodyContent) {
  Expr *PrintStr = new (Context) StringLiteralExpr(Str, Loc);
  Expr *PrintStrFn = OverloadedDeclRefExpr::createWithCopy(PrintDecls, Loc);
  BodyContent.push_back(new (Context) CallExpr(PrintStrFn, PrintStr));
}

static void
PrintReplExpr(TypeChecker &TC, VarDecl *Arg, CanType T, SourceLoc Loc,
              SourceLoc EndLoc,
              SmallVectorImpl<unsigned> &MemberIndexes,
              SmallVectorImpl<BraceStmt::ExprStmtOrDecl> &BodyContent,
              SmallVectorImpl<ValueDecl*> &PrintDecls) {
  ASTContext &Context = TC.Context;
  TranslationUnit &TU = TC.TU;

  if (TupleType *TT = dyn_cast<TupleType>(T)) {
    // We print a tuple by printing each element.
    PrintLiteralString("(", Context, Loc, PrintDecls, BodyContent);

    for (unsigned i = 0, e = TT->getFields().size(); i < e; ++i) {
      MemberIndexes.push_back(i);
      CanType SubType = TT->getElementType(i)->getCanonicalType();
      PrintReplExpr(TC, Arg, SubType, Loc, EndLoc, MemberIndexes,
                    BodyContent, PrintDecls);
      MemberIndexes.pop_back();

      if (i + 1 != e)
        PrintLiteralString(", ", Context, Loc, PrintDecls, BodyContent);
    }

    PrintLiteralString(")", Context, Loc, PrintDecls, BodyContent);
    return;
  }

  Identifier MemberName = Context.getIdentifier("replPrint");
  MemberLookup Lookup(T, MemberName, TU);
  if (Lookup.isSuccess()) {
    Expr *ArgRef = new (Context) DeclRefExpr(Arg, Loc,
                                             Arg->getTypeOfReference());
    ArgRef = TC.convertToRValue(ArgRef);
    for (unsigned i : MemberIndexes) {
      // For each index, we look through a TupleType or StructType.
      CanType CurT = ArgRef->getType()->getCanonicalType();
      if (StructType *ST = dyn_cast<StructType>(CurT)) {
        VarDecl *VD = cast<VarDecl>(ST->getDecl()->getMembers()[i]);
        ArgRef = new (Context) MemberRefExpr(ArgRef, Loc, VD, Loc);
        ArgRef = TC.recheckTypes(ArgRef);
        continue;
      }
      TupleType *TT = cast<TupleType>(CurT);
      ArgRef = new (Context) SyntacticTupleElementExpr(ArgRef, Loc, i, Loc,
                                                       TT->getElementType(i));
    }
    Expr *Res = TC.recheckTypes(Lookup.createResultAST(ArgRef, Loc, EndLoc,
                                                       Context));
    if (!Res)
      return;
    TupleExpr *CallArgs =
        new (Context) TupleExpr(Loc, MutableArrayRef<Expr *>(), 0, EndLoc,
                                TupleType::getEmpty(Context));
    CallExpr *CE = new (Context) CallExpr(Res, CallArgs, Type());
    Res = TC.semaApplyExpr(CE);
    if (!Res)
      return;
    BodyContent.push_back(Res);
    return;
  }

  if (StructType *ST = dyn_cast<StructType>(T)) {
    // We print a struct by printing each non-property member variable.
    PrintLiteralString(ST->getDecl()->getName().str(), Context, Loc,
                       PrintDecls, BodyContent);
    PrintLiteralString("(", Context, Loc, PrintDecls, BodyContent);

    unsigned idx = 0;
    bool isFirstMember = true;
    for (Decl *D : ST->getDecl()->getMembers()) {
      if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
        if (!VD->isProperty()) {
          if (isFirstMember)
            isFirstMember = false;
          else
            PrintLiteralString(", ", Context, Loc, PrintDecls, BodyContent);

          MemberIndexes.push_back(idx);
          CanType SubType = VD->getType()->getCanonicalType();
          PrintReplExpr(TC, Arg, SubType, Loc, EndLoc, MemberIndexes,
                        BodyContent, PrintDecls);
          MemberIndexes.pop_back();
        }
      }
      ++idx;
    }
    PrintLiteralString(")", Context, Loc, PrintDecls, BodyContent);
    return;
  }

  // FIXME: Handle ClassTypes?

  // FIXME: We should handle OneOfTypes at some point, but
  // it's tricky to represent in the AST without a "match" statement.

  PrintLiteralString("<unprintable value>", Context, Loc, PrintDecls,
                     BodyContent);
}

/// Check an expression at the top level in a REPL.
void TypeChecker::typeCheckTopLevelReplExpr(Expr *&E, TopLevelCodeDecl *TLCD) {
  // If the input is an lvalue, force an lvalue-to-rvalue conversion.
  Expr *ConvertedE = convertToMaterializable(E);
  if (!ConvertedE)
    return;
  E = ConvertedE;

  CanType T = E->getType()->getCanonicalType();
  SourceLoc Loc = E->getStartLoc();
  SourceLoc EndLoc = E->getEndLoc();

  // Don't try to print invalid expressions.
  if (isa<ErrorType>(T))
    return;

  // Skip printing for expressions of void type.
  if (isa<TupleType>(T) && cast<TupleType>(T)->getFields().empty())
    return;

  // Build a function to call to print the expression.
  Type FuncTy = T;
  if (!isa<TupleType>(FuncTy)) {
    TupleTypeElt Elt(T, Context.getIdentifier("arg"));
    FuncTy = TupleType::get(Elt, Context);
  }
  FuncTy = FunctionType::get(FuncTy, TupleType::getEmpty(Context), Context);
  VarDecl *Arg = new (Context) VarDecl(Loc, Context.getIdentifier("arg"), T,
                                       nullptr);
  Pattern* ParamPat = new (Context) NamedPattern(Arg);
  FuncExpr *FE = FuncExpr::create(Context, Loc, ParamPat, FuncTy, 0, TLCD);
  Arg->setDeclContext(FE);

  // Build the body of the function which prints the expression.
  SmallVector<unsigned, 4> MemberIndexes;
  SmallVector<BraceStmt::ExprStmtOrDecl, 4> BodyContent;
  SmallVector<ValueDecl*, 4> PrintDecls;
  UnqualifiedLookup PrintDeclLookup(Context.getIdentifier("print"), &TU);
  if (!PrintDeclLookup.isSuccess())
    return;
  for (auto Result : PrintDeclLookup.Results)
    PrintDecls.push_back(Result.getValueDecl());

  // Printing format is "Int = 0\n".
  auto TypeStr = Context.getIdentifier(E->getType()->getString()).str();
  PrintLiteralString(TypeStr, Context, Loc, PrintDecls, BodyContent);
  PrintLiteralString(" = ", Context, Loc, PrintDecls, BodyContent);
  PrintReplExpr(*this, Arg, T, Loc, EndLoc, MemberIndexes, BodyContent,
                PrintDecls);
  PrintLiteralString("\n", Context, Loc, PrintDecls, BodyContent);

  // Typecheck the function.
  BraceStmt *Body = BraceStmt::create(Context, Loc, BodyContent, EndLoc);
  FE->setBody(Body);
  typeCheckFunctionBody(FE);

  // Typecheck the call.
  CallExpr *CE = new (Context) CallExpr(FE, E);
  E = semaApplyExpr(CE);
}

struct PatternBindingPrintLHS : public ASTVisitor<PatternBindingPrintLHS> {
  SmallVectorImpl<BraceStmt::ExprStmtOrDecl> &BodyContent;
  SmallVectorImpl<ValueDecl*> &PrintDecls;
  SourceLoc Loc;
  ASTContext &Context;

  PatternBindingPrintLHS(SmallVectorImpl<BraceStmt::ExprStmtOrDecl>&BodyContent,
                         SmallVectorImpl<ValueDecl*> &PrintDecls,
                         SourceLoc Loc,
                         ASTContext &Context)
    : BodyContent(BodyContent), PrintDecls(PrintDecls), Loc(Loc),
      Context(Context) {}

  void print(StringRef Str) {
    PrintLiteralString(Str, Context, Loc, PrintDecls, BodyContent);
  }

  void visitTuplePattern(TuplePattern *P) {
    // We print tuples as "(x, y)".
    print("(");
    for (unsigned i = 0, e = P->getNumFields(); i != e; ++i) {
      visit(P->getFields()[i].getPattern());
      if (i + 1 != e)
        print(", ");
    }
    print(")");
  }
  void visitNamedPattern(NamedPattern *P) {
    print(P->getBoundName().str());
  }
  void visitAnyPattern(AnyPattern *P) {
    print("_");
  }

  void visitTypedPattern(TypedPattern *P) {
    // We prefer to print the type separately.
    visit(P->getSubPattern());
  }
  void visitParenPattern(ParenPattern *P) {
    // Don't print parentheses: they break the correspondence with the
    // way we print the expression.
    visit(P->getSubPattern());
  }
};

void TypeChecker::REPLCheckPatternBinding(PatternBindingDecl *D) {
  Expr *E = D->getInit();

  // FIXME: I'm assuming we don't need to bother printing the pattern binding
  // if it's a null initialization.
  if (!E)
    return;

  CanType T = E->getType()->getCanonicalType();
  SourceLoc Loc = E->getStartLoc();
  SourceLoc EndLoc = E->getEndLoc();

  SmallVector<ValueDecl*, 4> PrintDecls;
  UnqualifiedLookup PrintDeclLookup(Context.getIdentifier("print"), &TU);
  if (!PrintDeclLookup.isSuccess())
    return;
  for (auto Result : PrintDeclLookup.Results)
    PrintDecls.push_back(Result.getValueDecl());

  // Build function of type T->T which prints the operand.
  Type FuncTy = T;
  if (!isa<TupleType>(FuncTy)) {
    TupleTypeElt Elt(T, Context.getIdentifier("arg"));
    FuncTy = TupleType::get(Elt, Context);
  }
  FuncTy = FunctionType::get(FuncTy, FuncTy, Context);
  VarDecl *Arg = new (Context) VarDecl(Loc, Context.getIdentifier("arg"), T,
                                       nullptr);
  Pattern* ParamPat = new (Context) NamedPattern(Arg);
  FuncExpr *FE = FuncExpr::create(Context, Loc, ParamPat, FuncTy, 0, &TU);
  Arg->setDeclContext(FE);
  
  // Fill in body of function.
  SmallVector<BraceStmt::ExprStmtOrDecl, 4> BodyContent;
  PatternBindingPrintLHS PatPrinter(BodyContent, PrintDecls, Loc, Context);
  PatPrinter.visit(D->getPattern());
  PrintLiteralString(" : ", Context, Loc, PrintDecls, BodyContent);
  auto TypeStr = Context.getIdentifier(E->getType()->getString()).str();
  PrintLiteralString(TypeStr, Context, Loc, PrintDecls, BodyContent);
  PrintLiteralString(" = ", Context, Loc, PrintDecls, BodyContent);
  SmallVector<unsigned, 4> MemberIndexes;
  PrintReplExpr(*this, Arg, T, Loc, EndLoc, MemberIndexes, BodyContent,
                PrintDecls);
  PrintLiteralString("\n", Context, Loc, PrintDecls, BodyContent);
  Expr *ArgRef = new (Context) DeclRefExpr(Arg, Loc, Arg->getTypeOfReference());
  BodyContent.push_back(new (Context) ReturnStmt(Loc, ArgRef));

  // Typecheck the function.
  BraceStmt *Body = BraceStmt::create(Context, Loc, BodyContent, EndLoc);
  FE->setBody(Body);
  typeCheckFunctionBody(FE);

  CallExpr *CE = new (Context) CallExpr(FE, E);
  D->setInit(semaApplyExpr(CE));
}
