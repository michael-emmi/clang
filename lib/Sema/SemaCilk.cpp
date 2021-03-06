//===--- SemaCilk.cpp - Semantic Analysis for Cilk Plus -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// \brief This file implements Cilk Plus related semantic analysis.
///
//===----------------------------------------------------------------------===//
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/SemaInternal.h"

using namespace clang;
using namespace sema;

namespace {
typedef llvm::SmallVectorImpl<VarDecl const *> VarDeclVec;

// This visitor looks for references to the loop control variable inside a
// _Cilk_for body.
class CilkForControlVarVisitor
    : public RecursiveASTVisitor<CilkForControlVarVisitor> {
private:

  // This visitor looks for potential errors and warnings about the modification
  // of the loop control variable.
  class ControlVarUsageVisitor
      : public RecursiveASTVisitor<ControlVarUsageVisitor> {
  public:
    bool Error;

    // Constructor
    ControlVarUsageVisitor(Sema &S, DeclRefExpr *LCV, const ParentMap &P,
                           bool AddressOf)
        : Error(false), S(S), CurLCV(LCV), LCVDecl(CurLCV->getDecl()), PMap(P),
          AddressOf(AddressOf) {}

    // Check if the type is a pointer/reference to const data. If not, emit
    // diagnostic
    void CheckTypeAndDiagnose(QualType Ty, const SourceLocation &Loc,
                              unsigned DiagID, llvm::StringRef Param = "") {
      if ((Ty->isReferenceType() || (Ty->isPointerType() && AddressOf)) &&
          !Ty->getPointeeType().isConstQualified()) {
        if (!Param.empty())
          S.Diag(Loc, DiagID) << Param;
        else
          S.Diag(Loc, DiagID);
        S.Diag(LCVDecl->getLocation(),
               diag::note_cilk_for_loop_control_var_declared_here);
      }
    }

    // Detect cases where the LCV is directly being assigned or an alias being
    // created
    bool VisitBinaryOperator(BinaryOperator *BO) {
      Expr *LHS =
          BO->getLHS()->IgnoreImpCasts()->IgnoreParenNoopCasts(S.Context);
      if (BO->isAssignmentOp()) {
        if (CurLCV == LHS) {
          S.Diag(BO->getLocStart(),
                 diag::err_cilk_for_loop_modifies_control_var);
          S.Diag(LCVDecl->getLocation(),
                 diag::note_cilk_for_loop_control_var_declared_here);
          Error = true;
        } else {
          CheckTypeAndDiagnose(BO->getType(), BO->getLocStart(),
                               diag::warn_cilk_for_loop_control_var_aliased,
                               LCVDecl->getName());
        }
      }
      return true;
    }

    // Detect cases of variable declarations that declare an alias to the LCV
    bool VisitVarDecl(VarDecl *VD) {
      CheckTypeAndDiagnose(VD->getType(), VD->getLocation(),
                           diag::warn_cilk_for_loop_control_var_aliased,
                           LCVDecl->getName());
      return true;
    }

    // Detect cases where a function call can modify the loop control variable
    bool VisitCallExpr(CallExpr *Call) {
      Stmt *P = PMap.getParent(CurLCV);

      // If we took the address of the LCV, ignore the &
      if (AddressOf)
        P = PMap.getParent(P);

      if (CastExpr *CE = dyn_cast<CastExpr>(P)) {
        // Look at the very top level cast to ignore any non-relevant casts
        CastExpr *TopLevelCast = CE;
        while (CastExpr *C = dyn_cast<CastExpr>(PMap.getParent(TopLevelCast))) {
          TopLevelCast = C;
        }

        CastKind Kind = TopLevelCast->getCastKind();
        QualType CastTy = TopLevelCast->getType();

        bool PtrOrRefConstQualified = false;
        if (AddressOf)
          PtrOrRefConstQualified = CastTy->getPointeeType().isConstQualified();
        else
          PtrOrRefConstQualified = CastTy.isConstQualified();

        if (Kind != CK_LValueToRValue &&
            !((Kind == CK_NoOp || Kind == CK_BitCast) &&
              PtrOrRefConstQualified)) {
          S.Diag(CurLCV->getLocation(),
                 diag::warn_cilk_for_loop_control_var_func);
          S.Diag(LCVDecl->getLocation(),
                 diag::note_cilk_for_loop_control_var_declared_here);
        }
      } else {
        // If its not a cast expression, we need to check which parameter(s)
        // can modify the loop control variable because in a rare case of the
        // comma operator where code like:
        //   int *p = (func(0), &i);
        // will generate a false positive in the 'func(0)' function call
        for (unsigned i = 0, len = Call->getNumArgs(); i != len; ++i) {
          Expr *Arg = Call->getArg(i)->IgnoreImpCasts()
              ->IgnoreParenNoopCasts(S.Context);

          // Remove the & from the argument so that we can compare the argument
          // with the LCV to see if they are the same
          if (UnaryOperator *UO = dyn_cast<UnaryOperator>(Arg)) {
            if (UO->getOpcode() == UO_AddrOf)
              Arg = UO->getSubExpr();
          }
          if (Arg == CurLCV) {
            S.Diag(CurLCV->getLocation(),
                   diag::warn_cilk_for_loop_control_var_func);
            S.Diag(LCVDecl->getLocation(),
                   diag::note_cilk_for_loop_control_var_declared_here);
          }
        }
      }

      return true;
    }

    // Detect cases of ++/--.
    bool VisitUnaryOperator(UnaryOperator *UO) {
      if (UO->isIncrementDecrementOp() && UO->getSubExpr() == CurLCV) {
        S.Diag(UO->getLocStart(), diag::err_cilk_for_loop_modifies_control_var);
        S.Diag(LCVDecl->getLocation(),
               diag::note_cilk_for_loop_control_var_declared_here);
        Error = true;
      }

      return true;
    }

  private:
    Sema &S;
    DeclRefExpr *CurLCV;      // Reference to the current loop control var
    const ValueDecl *LCVDecl; // The declaration of the current loop control var
    const ParentMap &PMap;
    bool AddressOf;
  };

public:
  bool Error;

  // Constructor
  CilkForControlVarVisitor(Sema &S, const ParentMap &PM, const VarDeclVec &LCVs)
      : Error(false), S(S), PMap(PM), LoopControlVarsInScope(LCVs) {}

  // Checks if the given DeclRefExpr is a reference to a loop control variable
  // in scope
  bool IsValidLoopControlVar(const DeclRefExpr *DeclRef) {
    VarDeclVec::const_iterator LCV =
        std::find(LoopControlVarsInScope.begin(), LoopControlVarsInScope.end(),
                  DeclRef->getDecl());
    return LCV != LoopControlVarsInScope.end();
  }

  bool VisitDeclRefExpr(DeclRefExpr *DeclRef) {
    if (IsValidLoopControlVar(DeclRef)) {
      Stmt *P = PMap.getParentIgnoreParenImpCasts(DeclRef);
      bool AddressOf = false;
      // Check to see if the address of the loop control variable was taken and
      // strip off any casts from its parent.
      if (UnaryOperator *UO = dyn_cast<UnaryOperator>(P)) {
        if (UO->getOpcode() == UO_AddrOf) {
          AddressOf = true;
          P = PMap.getParentIgnoreParenImpCasts(P);
          while (dyn_cast<CastExpr>(P)) {
            P = PMap.getParentIgnoreParenImpCasts(P);
          }

          // If the parent is a comma operator, get its parent to see if there
          // are any assignments.
          if (BinaryOperator *BO = dyn_cast<BinaryOperator>(P)) {
            if (BO->getOpcode() == BO_Comma)
              P = PMap.getParentIgnoreParenImpCasts(P);
          }
        }
      }

      // Use the usage visitor to analyze if the parent tries to modify the
      // loop control variable
      ControlVarUsageVisitor V(S, DeclRef, PMap, AddressOf);
      V.TraverseStmt(P);
      Error |= V.Error;
    }
    return true;
  }

private:
  Sema &S;
  const ParentMap &PMap;
  const VarDeclVec &LoopControlVarsInScope;
};

} // namespace

static bool CheckForInit(Sema &S, Stmt *Init,
                         VarDecl *&ControlVar, Expr *&ControlVarInit,
                         bool IsCilkFor) {
  // Location of loop control variable/expression in the initializer
  SourceLocation InitLoc;

  if (DeclStmt *DS = dyn_cast<DeclStmt>(Init)) {
    // The initialization shall declare or initialize a single variable,
    // called the control variable.
    if (!DS->isSingleDecl()) {
      DeclStmt::decl_iterator DI = DS->decl_begin();
      ++DI;
      S.Diag((*DI)->getLocation(),
             IsCilkFor ? diag::err_cilk_for_decl_multiple_variables
                       : diag::err_simd_for_decl_multiple_variables);
      return false;
    }

    ControlVar = dyn_cast<VarDecl>(*DS->decl_begin());
    // Only allow VarDecls in the initializer
    if (!ControlVar) {
      S.Diag(Init->getLocStart(),
             IsCilkFor ? diag::err_cilk_for_initializer_expected_decl
                       : diag::err_simd_for_initializer_expected_decl)
        << Init->getSourceRange();
      return false;
    }

    // Ignore invalid decls.
    if (ControlVar->isInvalidDecl())
      return false;

    // The control variable shall be declared and initialized within the
    // initialization clause of the _Cilk_for loop.

    // A template type may have a default constructor.
    bool IsDependent = ControlVar->getType()->isDependentType();
    if (!IsDependent && !ControlVar->getInit()) {
      S.Diag(ControlVar->getLocation(),
             IsCilkFor ? diag::err_cilk_for_control_variable_not_initialized
                       : diag::err_simd_for_control_variable_not_initialized);
      return false;
    }

    InitLoc = ControlVar->getLocation();
    ControlVarInit = ControlVar->getInit();
  } else {
    // In C++, the control variable shall be declared and initialized within
    // the initialization clause of the _Cilk_for loop.
    if (S.getLangOpts().CPlusPlus) {
      S.Diag(Init->getLocStart(),
             IsCilkFor ? diag::err_cilk_for_initialization_must_be_decl
                       : diag::err_simd_for_initialization_must_be_decl);
      return false;
    }

    // In C only, the control variable may be previously declared, but if so
    // shall be reinitialized, i.e., assigned, in the initialization clause.
    BinaryOperator *Op = 0;
    if (Expr *E = dyn_cast<Expr>(Init)) {
      E = E->IgnoreParenNoopCasts(S.Context);
      Op = dyn_cast_or_null<BinaryOperator>(E);
    }

    if (!Op) {
      S.Diag(Init->getLocStart(),
             IsCilkFor ? diag::err_cilk_for_control_variable_not_initialized
                       : diag::err_simd_for_control_variable_not_initialized);
      return false;
    }

    // The initialization shall declare or initialize a single variable,
    // called the control variable.
    if (Op->getOpcode() == BO_Comma) {
      S.Diag(Op->getRHS()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_init_multiple_variables
                       : diag::err_simd_for_init_multiple_variables);
      return false;
    }

    if (!Op->isAssignmentOp()) {
      S.Diag(Op->getLHS()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_control_variable_not_initialized
                       : diag::err_simd_for_control_variable_not_initialized)
        << Init->getSourceRange();
      return false;
    }

    // Get the decl for the LHS of the control variable initialization
    assert(Op->getLHS() && "BinaryOperator has no LHS!");
    DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
      Op->getLHS()->IgnoreParenNoopCasts(S.Context));
    if (!LHS) {
      S.Diag(Op->getLHS()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_initializer_expected_variable
                       : diag::err_simd_for_expect_loop_control_variable)
        << Init->getSourceRange();
      return false;
    }

    // But, use the source location of the LHS for diagnostics
    InitLoc = LHS->getLocation();

    // Only a VarDecl may be used in the initializer
    ControlVar = dyn_cast<VarDecl>(LHS->getDecl());
    if (!ControlVar) {
      S.Diag(Op->getLHS()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_initializer_expected_variable
                       : diag::err_simd_for_expect_loop_control_variable)
        << Init->getSourceRange();
      return false;
    }
    ControlVarInit = Op->getRHS();
  }

  bool IsDeclStmt = isa<DeclStmt>(Init);

  // No storage class may be specified for the variable within the
  // initialization clause.
  StorageClass SC = ControlVar->getStorageClass();
  if (SC != SC_None) {
    S.Diag(InitLoc,
           IsCilkFor ? diag::err_cilk_for_control_variable_storage_class
                     : diag::err_simd_for_control_variable_storage_class)
      << ControlVar->getStorageClassSpecifierString(SC);
    if (!IsDeclStmt)
      S.Diag(ControlVar->getLocation(), diag::note_local_variable_declared_here)
        << ControlVar->getIdentifier();
    return false;
  }

  // Don't allow non-local variables to be used as the control variable
  if (!ControlVar->isLocalVarDecl()) {
    S.Diag(InitLoc,
           IsCilkFor ? diag::err_cilk_for_control_variable_not_local
                     : diag::err_simd_for_control_variable_not_local);
    return false;
  }

  QualType VarType = ControlVar->getType();

  // For decltype types, get the actual type
  const Type *VarTyPtr = VarType.getTypePtrOrNull();
  if (VarTyPtr && isa<DecltypeType>(VarTyPtr))
    VarType = cast<DecltypeType>(VarTyPtr)->getUnderlyingType();

  // The variable may not be const or volatile.
  // Assignment to const variables is checked before sema for cilk_for
  if (VarType.isVolatileQualified()) {
    S.Diag(InitLoc,
           IsCilkFor ? diag::err_cilk_for_control_variable_qualifier
                     : diag::err_simd_for_control_variable_qualifier)
      << "volatile";
    if (!IsDeclStmt)
      S.Diag(ControlVar->getLocation(), diag::note_local_variable_declared_here)
        << ControlVar->getIdentifier();
    return false;
  }

  if (IsCilkFor) {
    // The variable shall have integral, pointer, or class type.
    // struct/class types only allowed in C++. Defer the type check for
    // a dependent type.
    if (!VarType->isDependentType()) {
      bool ValidType = false;
      if (S.getLangOpts().CPlusPlus &&
          (VarTyPtr->isClassType() || VarTyPtr->isStructureType() ||
           VarTyPtr->isUnionType()))
        ValidType = true;
      else if (VarTyPtr->isIntegralType(S.Context) || VarTyPtr->isPointerType())
        ValidType = true;

      if (!ValidType) {
        S.Diag(InitLoc, diag::err_cilk_for_control_variable_type);
        if (!IsDeclStmt)
          S.Diag(ControlVar->getLocation(),
                 diag::note_local_variable_declared_here)
            << ControlVar->getIdentifier();
        return false;
      }
    }
  } else {
    // simd for -- the variable shall have integer or pointer type.
    bool ValidTy = VarType->isIntegerType() || VarType->isPointerType();
    if (VarType->isAggregateType() || VarType->isReferenceType()
                                   || (!ValidTy &&
                                       !VarType->isDependentType())) {
      S.Diag(InitLoc, diag::err_simd_for_invalid_lcv_type)
        << VarType;
      if (!IsDeclStmt)
        S.Diag(ControlVar->getLocation(),
               diag::note_local_variable_declared_here)
          << ControlVar->getIdentifier();
      return false;
    }
  }

  return true;
}

static bool ExtractForCondition(Sema &S,
                                Expr *Cond,
                                BinaryOperatorKind &CondOp,
                                SourceLocation &OpLoc,
                                Expr *&LHS,
                                Expr *&RHS,
                                bool IsCilkFor) {
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(Cond)) {
    CondOp = BO->getOpcode();
    OpLoc = BO->getOperatorLoc();
    LHS = BO->getLHS();
    RHS = BO->getRHS();
    return true;
  } else if (CXXOperatorCallExpr *OO = dyn_cast<CXXOperatorCallExpr>(Cond)) {
    CondOp = BinaryOperator::getOverloadedOpcode(OO->getOperator());
    if (OO->getNumArgs() == 2) {
      OpLoc = OO->getOperatorLoc();
      LHS = OO->getArg(0);
      RHS = OO->getArg(1);
      return true;
    }
  } else if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(Cond)) {
    if (IsCilkFor) {
      if (ICE->getCastKind() == CK_UserDefinedConversion) {
        Expr *From = ICE->getSubExprAsWritten();
        // Note: flags copied from TryContextuallyConvertToBool
        ImplicitConversionSequence ICS =
            S.TryImplicitConversion(From, ICE->getType(),
                                    /*SuppressUserConversions=*/false,
                                    /*AllowExplicit=*/true,
                                    /*InOverloadResolution=*/false,
                                    /*CStyle=*/false,
                                    /*AllowObjCWritebackConversion=*/false);
        assert(!ICS.isBad() && ICS.getKind() ==
                             ImplicitConversionSequence::UserDefinedConversion);
        S.Diag(Cond->getExprLoc(), diag::warn_cilk_for_cond_user_defined_conv)
          << From->getType() << ICE->getType() << Cond->getSourceRange();
        FunctionDecl *FD = ICS.UserDefined.ConversionFunction->getCanonicalDecl();
        S.Diag(FD->getLocation(), diag::note_cilk_for_conversion_here)
          << ICE->getType();
      }
      Cond = ICE->getSubExpr();
    } else
      Cond = ICE->IgnoreImpCastsAsWritten();
    return ExtractForCondition(S, Cond, CondOp, OpLoc, LHS, RHS, IsCilkFor);
  } else if (CXXMemberCallExpr *MC = dyn_cast<CXXMemberCallExpr>(Cond)) {
    CXXMethodDecl *MD = MC->getMethodDecl();
    if (isa<CXXConversionDecl>(MD))
      return ExtractForCondition(S, MC->getImplicitObjectArgument(), CondOp,
                                 OpLoc, LHS, RHS, IsCilkFor);
  } else if (CXXBindTemporaryExpr *BT = dyn_cast<CXXBindTemporaryExpr>(Cond)) {
    return ExtractForCondition(S, BT->getSubExpr(), CondOp, OpLoc, LHS, RHS,
                               IsCilkFor);
  } else if (ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(Cond))
    return ExtractForCondition(S, EWC->getSubExpr(), CondOp, OpLoc, LHS, RHS,
                               IsCilkFor);

  S.Diag(Cond->getExprLoc(), IsCilkFor ? diag::err_cilk_for_invalid_cond_expr
                                       : diag::err_simd_for_invalid_cond_expr)
    << Cond->getSourceRange();
  return false;
}

static bool IsControlVarRef(Expr *E, const VarDecl *ControlVar) {
  // Only ignore very basic casts and this allows us to distinguish
  //
  // struct Int {
  //  Int();
  //  operator int&();
  // };
  //
  // _Cilk_for (Int i; i.opertor int&() < 10; ++i);
  //
  // and
  //
  // _Cilk_for (Int i; i < 10; ++i);
  //
  // The first is a member function call and the second is also member function
  // call but associated with a user defined conversion cast.
  //
  E = E->IgnoreParenLValueCasts();

  if (CXXConstructExpr *C = dyn_cast<CXXConstructExpr>(E)) {
    if (C->getConstructor()->isConvertingConstructor(false))
      return IsControlVarRef(C->getArg(0), ControlVar);
  } else if (CXXBindTemporaryExpr *BE = dyn_cast<CXXBindTemporaryExpr>(E)) {
    return IsControlVarRef(BE->getSubExpr(), ControlVar);
  } else if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    // Apply recursively with the subexpression written in the source.
    return IsControlVarRef(ICE->getSubExprAsWritten(), ControlVar);
  } else if (DeclRefExpr *DR = dyn_cast<DeclRefExpr>(E))
    if (DR->getDecl() == ControlVar)
      return true;

  return false;
}

static bool CanonicalizeForCondOperands(Sema &S, const VarDecl *ControlVar,
                                        Expr *Cond, Expr *&LHS,
                                        Expr *&RHS, int &Direction,
                                        unsigned CondDiagError,
                                        unsigned CondDiagNote) {

  // The condition shall have one of the following two forms:
  //   var OP shift-expression
  //   shift-expression OP var
  // where var is the control variable, optionally enclosed in parentheses.
  if (IsControlVarRef(LHS, ControlVar))
    return true;

  if (IsControlVarRef(RHS, ControlVar)) {
    std::swap(LHS, RHS);
    Direction = -Direction;
    return true;
  }

  S.Diag(Cond->getLocStart(), CondDiagError) << ControlVar
    << Cond->getSourceRange();
  S.Diag(Cond->getLocStart(), CondDiagNote) << ControlVar;
  return false;
}

static void CheckForCondition(Sema &S,
                              VarDecl *ControlVar,
                              Expr *Cond,
                              Expr *&Limit,
                              int &Direction,
                              BinaryOperatorKind &Opcode,
                              bool IsCilkFor) {
  SourceLocation OpLoc;
  Expr *LHS = 0;
  Expr *RHS = 0;

  Cond = Cond->IgnoreParens();
  if (!ExtractForCondition(S, Cond, Opcode, OpLoc, LHS, RHS, IsCilkFor))
    return;

  // The operator denoted OP shall be one of !=, <=, <, >=, or >.
  switch (Opcode) {
  case BO_NE:
    Direction = 0;
    break;
  case BO_LT:
  case BO_LE:
    Direction = 1;
    break;
  case BO_GT:
  case BO_GE:
    Direction = -1;
    break;
  default:
    S.Diag(OpLoc, IsCilkFor ? diag::err_cilk_for_invalid_cond_operator
                            : diag::err_simd_for_invalid_cond_operator);
    return;
  }
  unsigned CondDiagError = IsCilkFor ? diag::err_cilk_for_cond_test_control_var
                                     : diag::err_simd_for_cond_test_control_var;
  unsigned CondDiagNote = IsCilkFor ? diag::note_cilk_for_cond_allowed
                                    : diag::note_simd_for_cond_allowed;
  if (!CanonicalizeForCondOperands(S, ControlVar, Cond, LHS, RHS, Direction,
                                   CondDiagError, CondDiagNote))
    return;

  Limit = RHS;
}

static bool IsValidForIncrement(Sema &S, Expr *Increment,
                                const VarDecl *ControlVar,
                                bool &HasConstantIncrement,
                                llvm::APSInt &Stride, Expr *&StrideExpr,
                                SourceLocation &RHSLoc,
                                bool IsCilkFor) {
  Increment = Increment->IgnoreParens();
  if (ExprWithCleanups *E = dyn_cast<ExprWithCleanups>(Increment))
    Increment = E->getSubExpr();
  if (CXXBindTemporaryExpr *E = dyn_cast<CXXBindTemporaryExpr>(Increment))
    Increment = E->getSubExpr();

  // Simple increment or decrement -- always OK
  if (UnaryOperator *U = dyn_cast<UnaryOperator>(Increment)) {
    if (!IsControlVarRef(U->getSubExpr(), ControlVar)) {
      S.Diag(U->getSubExpr()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_increment_not_control_var
                       : diag::err_simd_for_increment_not_control_var)
             << ControlVar;
       return false;
    }

    if (U->isIncrementDecrementOp()) {
      HasConstantIncrement = true;
      Stride = (U->isIncrementOp() ? 1 : -1);
      StrideExpr = S.ActOnIntegerConstant(Increment->getExprLoc(), 1).get();
      if (U->isDecrementOp())
        StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                    UO_Minus, StrideExpr).get();
      return true;
    }
  }

  // In the case of += or -=, whether built-in or overloaded, we need to check
  // the type of the right-hand side. In that case, RHS will be set to a
  // non-null value.
  Expr *RHS = 0;
  // Direction is 1 if the operator is +=, -1 if it is -=
  int Direction = 0;
  StringRef OperatorName;

  if (CXXOperatorCallExpr *C = dyn_cast<CXXOperatorCallExpr>(Increment)) {
    OverloadedOperatorKind Overload = C->getOperator();

    if (!IsControlVarRef(C->getArg(0), ControlVar)) {
      S.Diag(C->getArg(0)->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_increment_not_control_var
                       : diag::err_simd_for_increment_not_control_var)
             << ControlVar;
      return false;
    }

    // operator++() or operator--() -- always OK
    if (Overload == OO_PlusPlus || Overload == OO_MinusMinus) {
      HasConstantIncrement = true;
      Stride = (Overload == OO_PlusPlus ? 1 : -1);
      StrideExpr = S.ActOnIntegerConstant(Increment->getExprLoc(), 1).get();
      if (Overload == OO_MinusMinus)
        StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                    UO_Minus, StrideExpr).get();
      return true;
    }

    // operator+=() or operator-=() -- defer checking of the RHS type
    if (Overload == OO_PlusEqual || Overload == OO_MinusEqual) {
      RHS = C->getArg(1);
      OperatorName = (Overload == OO_PlusEqual ? "+=" : "-=");
      Direction = Overload == OO_PlusEqual ? 1 : -1;
    }
  }

  if (BinaryOperator *B = dyn_cast<CompoundAssignOperator>(Increment)) {
    if (!IsControlVarRef(B->getLHS(), ControlVar)) {
      S.Diag(B->getLHS()->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_increment_not_control_var
                       : diag::err_simd_for_increment_not_control_var)
             << ControlVar;
      return false;
    }

    // += or -= -- defer checking of the RHS type
    if (B->isAdditiveAssignOp()) {
      RHS = B->getRHS();
      OperatorName = B->getOpcodeStr();
      Direction = B->getOpcode() == BO_AddAssign ? 1 : -1;
    }
  }

  // If RHS is non-null, it's a += or -=, either built-in or overloaded.
  // We need to check that the RHS has the correct type.
  if (RHS) {
    if (RHS->isTypeDependent())
      return true;

    if (!RHS->getType()->isIntegralOrEnumerationType()) {
      S.Diag(Increment->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_invalid_increment_rhs
                       : diag::err_simd_for_invalid_increment_rhs)
             << OperatorName;
      return false;
    }

    // Handle the case like 'RHS = sizeof(T)', which is not type dependent
    // but value dependent.
    if (RHS->isValueDependent())
      return true;

    HasConstantIncrement = RHS->EvaluateAsInt(Stride, S.Context);
    StrideExpr = RHS;
    if (Direction == -1) {
      Stride = -Stride;
      StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                  UO_Minus, StrideExpr).get();
    }
    RHSLoc = RHS->getExprLoc();
    return true;
  }

  // If we reached this point, the basic form is invalid. Issue a diagnostic.
  S.Diag(Increment->getExprLoc(),
         IsCilkFor ? diag::err_cilk_for_invalid_increment
                   : diag::err_simd_for_invalid_increment);
  return false;
}

ExprResult
Sema::CalculateCilkForLoopCount(SourceLocation CilkForLoc, Expr *Span,
                                Expr *Increment, Expr *StrideExpr, int Dir,
                                BinaryOperatorKind Opcode) {
  // Generate the loop count expression according to the following:
  // ===========================================================================
  // |     Condition syntax             |       Loop count                     |
  // ===========================================================================
  // | if var < limit or limit > var    | (span-1)/stride + 1                  |
  // ---------------------------------------------------------------------------
  // | if var > limit or limit < var    | (span-1)/-stride + 1                 |
  // ---------------------------------------------------------------------------
  // | if var <= limit or limit >= var  | span/stride + 1                      |
  // ---------------------------------------------------------------------------
  // | if var >= limit or limit <= var  | span/-stride + 1                     |
  // ---------------------------------------------------------------------------
  // | if var != limit or limit != var  | if stride is positive,               |
  // |                                  |            span/stride               |
  // |                                  | otherwise, span/-stride              |
  // |                                  | We don't need "+(stride-1)" for the  |
  // |                                  | span in this case since the incr/decr|
  // |                                  | operator should add up to the        |
  // |                                  | limit exactly for a valid loop.      |
  // ---------------------------------------------------------------------------
  // Build "-stride"
  Expr *NegativeStride = BuildUnaryOp(getCurScope(), Increment->getExprLoc(),
                                      UO_Minus, StrideExpr).get();

  ExprResult LoopCount;
  if (Opcode == BO_NE) {
    if (StrideExpr->getType()->isSignedIntegerOrEnumerationType()) {
      // Build "stride<0"
      Expr *StrideLessThanZero =
          BuildBinOp(getCurScope(), CilkForLoc, BO_LT, StrideExpr,
                     ActOnIntegerConstant(CilkForLoc, 0).get()).get();
      // Build "(stride<0)?-stride:stride"
      ExprResult StrideCondExpr =
          ActOnConditionalOp(CilkForLoc, CilkForLoc, StrideLessThanZero,
                             NegativeStride, StrideExpr);
      // Build "-span"
      Expr *NegativeSpan =
          BuildUnaryOp(getCurScope(), CilkForLoc, UO_Minus, Span).get();

      // Updating span to be "(stride<0)?-span:span"
      Span = ActOnConditionalOp(CilkForLoc, CilkForLoc, StrideLessThanZero,
                                NegativeSpan, Span).get();
      // Build "span/(stride<0)?-stride:stride"
      LoopCount = BuildBinOp(getCurScope(), CilkForLoc, BO_Div, Span,
                             StrideCondExpr.get());
    } else
      // Unsigned, no need to compare - Build "span/stride"
      LoopCount =
          BuildBinOp(getCurScope(), CilkForLoc, BO_Div, Span, StrideExpr);

  } else {
    if (Opcode == BO_LT || Opcode == BO_GT)
      // Updating span to be "span-1"
      Span =
          CreateBuiltinBinOp(CilkForLoc, BO_Sub, Span,
                             ActOnIntegerConstant(CilkForLoc, 1).get()).get();

    // Build "span/stride" if Dir==1, otherwise "span/-stride"
    LoopCount =
      BuildBinOp(getCurScope(), CilkForLoc, BO_Div, Span,
                 (Dir == 1) ? StrideExpr : NegativeStride);

    // Build "span/stride + 1"
    LoopCount = BuildBinOp(getCurScope(), CilkForLoc, BO_Add, LoopCount.get(),
                           ActOnIntegerConstant(CilkForLoc, 1).get());
  }

  QualType LoopCountExprType = LoopCount.get()->getType();
  QualType LoopCountType = Context.UnsignedLongLongTy;
  // Loop count should be either u32 or u64 in Cilk Plus.
  if (Context.getTypeSize(LoopCountExprType) > 64)
    Diag(CilkForLoc, diag::warn_cilk_for_loop_count_downcast)
      << LoopCountExprType << LoopCountType;
  else if (Context.getTypeSize(LoopCountExprType) <= 32)
    LoopCountType = Context.UnsignedIntTy;

  // Implicitly casting LoopCount to u32/u64.
  return ImpCastExprToType(LoopCount.get(), LoopCountType, CK_IntegralCast);
}

StmtResult
Sema::ActOnCilkForGrainsizePragma(Expr *GrainsizeExpr, Stmt *CilkFor,
                                  SourceLocation LocStart) {
  SourceLocation GrainSizeStart = GrainsizeExpr->getLocStart();

  // Negative grainsize has unspecified behavior and is reserved for future
  // extensions.
  llvm::APSInt Result;
  if (GrainsizeExpr->EvaluateAsInt(Result, Context))
    if (Result.isNegative()) {
      Diag(GrainSizeStart, diag::err_cilk_for_grainsize_negative);
      return StmtError();
    }

  // Check if the result of the Grainsize expression is convertible to signed
  // int.
  QualType GrainsizeTy = Context.IntTy;
  VarDecl *Grainsize = VarDecl::Create(
      Context, CurContext, GrainSizeStart, GrainSizeStart, 0, GrainsizeTy,
      Context.getTrivialTypeSourceInfo(GrainsizeTy, GrainSizeStart), SC_None);

  AddInitializerToDecl(Grainsize, GrainsizeExpr, true, false);
  if (Grainsize->isInvalidDecl()) {
    Context.Deallocate(reinterpret_cast<void*>(Grainsize));
    Diag(GrainSizeStart, diag::note_cilk_for_grainsize_conversion) << GrainsizeTy;
    return StmtError();
  }

  GrainsizeExpr = Grainsize->getInit();
  Context.Deallocate(reinterpret_cast<void*>(Grainsize));
  return new (Context) CilkForGrainsizeStmt(GrainsizeExpr, CilkFor, LocStart);
}

static void CheckForSignedUnsignedWraparounds(const VarDecl *ControlVar,
                                              const Expr *ControlVarInit,
                                              const Expr *Limit, Sema &S,
                                              int CondDirection,
                                              llvm::APSInt Stride,
                                              const Expr *StrideExpr) {
  llvm::APSInt InitVal;
  llvm::APSInt LimitVal;
  if (!ControlVar->getType()->isDependentType() &&
      !ControlVarInit->isInstantiationDependent() &&
      !Limit->isInstantiationDependent()) {
    if (ControlVarInit->isIntegerConstantExpr(InitVal, S.Context) &&
        Limit->isIntegerConstantExpr(LimitVal, S.Context) &&
        CondDirection == 0) {
      // Make InitVal, LimitVal, and Stride have the same width
      // so that they can be compared.
      unsigned MaxBitWidth = std::max(std::max(InitVal.getMinSignedBits(),
                                               LimitVal.getMinSignedBits()),
                                      Stride.getMinSignedBits());
      InitVal = InitVal.sextOrTrunc(MaxBitWidth);
      LimitVal = LimitVal.sextOrTrunc(MaxBitWidth);
      Stride = Stride.sextOrTrunc(MaxBitWidth);
      InitVal.setIsSigned(true);
      LimitVal.setIsSigned(true);
      Stride.setIsSigned(true);

      const char *StrideSign = (Stride.isNegative() ? "negative" : "positive");
      const char *SignedUnsigned =
          (ControlVar->getType()->isUnsignedIntegerOrEnumerationType()
               ? "unsigned"
               : "signed");
      if ((Stride.isNegative() && InitVal < LimitVal) ||
          (Stride.isStrictlyPositive() && InitVal > LimitVal)) {
        S.Diag(StrideExpr->getExprLoc(), diag::warn_cilk_for_wraparound)
            << StrideSign << SignedUnsigned;
        S.Diag(StrideExpr->getExprLoc(),
               diag::note_cilk_for_wraparound_undefined);
      } else if (StrideExpr->isIntegerConstantExpr(S.Context)) {
        if (LimitVal % Stride != 0) {
          S.Diag(StrideExpr->getExprLoc(), diag::warn_cilk_for_wraparound)
              << StrideSign << SignedUnsigned;
          S.Diag(StrideExpr->getExprLoc(),
                 diag::note_cilk_for_wraparound_undefined);
        }
      }
    }
  }
}

bool Sema::CheckIfBodyModifiesLoopControlVar(Stmt *Body) {
  assert(!FunctionScopes.empty() && "Must be inside a _Cilk_for scope");

  llvm::SmallVector<VarDecl const *, 1> LoopControlVarsInScope;

  // Store all the _Cilk_for loop control vars upto the top most _Cilk_for
  for (SmallVectorImpl<sema::FunctionScopeInfo *>::reverse_iterator
           I = FunctionScopes.rbegin(),
           E = FunctionScopes.rend();
       I != E; ++I) {
    if (CilkForScopeInfo *CSI = dyn_cast<CilkForScopeInfo>(*I)) {
      LoopControlVarsInScope.push_back(CSI->LoopControlVar);
    }
  }

  ParentMap PMap(Body);
  CilkForControlVarVisitor V(*this, PMap, LoopControlVarsInScope);
  V.TraverseStmt(Body);

  return V.Error;
}

static bool CheckForIncrement(Sema &S, Expr *Increment, const Expr *Limit,
                              const VarDecl *ControlVar,
                              const Expr *ControlVarInit,
                              int CondDirection, Expr *&StrideExpr,
                              bool IsCilkFor) {
  // Check increment
  // For dependent types since we can't get the actual type, we default to a
  // 64bit signed type.
  llvm::APSInt Stride(64, true);
  // Otherwise, stride should be the same type as the control var. This only
  // matters because of the computation we do with the value of the loop limit
  // later.
  if (IsCilkFor && !ControlVar->getType()->isDependentType())
    Stride = llvm::APSInt(
        S.Context.getTypeSize(ControlVar->getType()),
        ControlVar->getType()->isUnsignedIntegerOrEnumerationType());

  bool HasConstantIncrement = false;
  SourceLocation IncrementRHSLoc;
  if (!IsValidForIncrement(S, Increment, ControlVar, HasConstantIncrement,
                           Stride, StrideExpr, IncrementRHSLoc, IsCilkFor))
    return false;

  // Check consistency between loop condition and increment only if the
  // increment amount is known at compile-time.
  if (HasConstantIncrement) {
    if (!Stride) {
      S.Diag(IncrementRHSLoc, IsCilkFor ? diag::err_cilk_for_increment_zero
                                        : diag::err_simd_for_increment_zero);
      return false;
    }

    if ((CondDirection > 0 && Stride.isNegative()) ||
        (CondDirection < 0 && Stride.isStrictlyPositive())) {
      S.Diag(Increment->getExprLoc(),
             IsCilkFor ? diag::err_cilk_for_increment_inconsistent
                       : diag::err_simd_for_increment_inconsistent)
             << (CondDirection > 0);
      S.Diag(Increment->getExprLoc(), diag::note_constant_stride)
        << Stride.toString(10, true)
        << SourceRange(Increment->getExprLoc(), Increment->getLocEnd());
      return false;
    }

    // For simd, do not check unsigned wrap around here, since OpenMP does not
    // support operation '!=' for the condition.
    if (IsCilkFor)
      CheckForSignedUnsignedWraparounds(ControlVar, ControlVarInit, Limit,
                                        S, CondDirection, Stride,
                                        StrideExpr);
  }

  return true;
}

// Find the loop control variable. Returns null if not found.
static const VarDecl *getLoopControlVariable(Sema &S, StmtResult InitStmt) {
  if (InitStmt.isInvalid())
    return 0;

  Stmt *Init = InitStmt.get();

  // No initialization.
  if (!Init)
    return 0;

  const VarDecl *Candidate = 0;

  // Initialization is a declaration statement.
  if (DeclStmt *DS = dyn_cast<DeclStmt>(Init)) {
    if (!DS->isSingleDecl())
      return 0;

    if (VarDecl *Var = dyn_cast<VarDecl>(DS->getSingleDecl()))
      Candidate = Var;
  } else {
    // Initialization is an expression.
    BinaryOperator *Op = 0;
    if (Expr *E = dyn_cast<Expr>(Init)) {
      E = E->IgnoreParenNoopCasts(S.Context);
      Op = dyn_cast<BinaryOperator>(E);
    }

    if (!Op || !Op->isAssignmentOp())
      return 0;

    Expr *E = Op->getLHS();
    if (!E)
      return 0;

    E = E->IgnoreParenNoopCasts(S.Context);
    DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(E);
    if (!LHS)
      return 0;

    if (VarDecl *Var = dyn_cast<VarDecl>(LHS->getDecl()))
      Candidate = Var;
  }

  // Only local variables can be a loop control variable.
  if (Candidate && Candidate->isLocalVarDecl())
    return Candidate;

  // Cannot find the loop control variable.
  return 0;
}

void Sema::ActOnStartOfCilkForStmt(SourceLocation CilkForLoc, Scope *CurScope,
                                   StmtResult FirstPart) {

  CapturedDecl *CD = 0;
  RecordDecl *RD = CreateCapturedStmtRecordDecl(CD, CilkForLoc, /*NumArgs*/3);

  const VarDecl *VD = getLoopControlVariable(*this, FirstPart);
  PushCilkForScope(CurScope, CD, RD, VD, CilkForLoc);

  // Push a compound scope for the body. This is needed for the case
  //
  // _Cilk_for (...)
  //   _Cilk_spawn foo();
  //
  PushCompoundScope();

  if (CurScope)
    PushDeclContext(CurScope, CD);
  else
    CurContext = CD;

  PushExpressionEvaluationContext(PotentiallyEvaluated);
}

namespace {

bool extendsLifetimeOfTemporary(const VarDecl *VD) {
  // Attempt to determine whether this declaration lifetime-extends a
  // temporary.
  //
  // FIXME: This is incorrect. Non-reference declarations can lifetime-extend
  // temporaries, and a single declaration can extend multiple temporaries.
  // We should look at the storage duration on each nested
  // MaterializeTemporaryExpr instead.
  //
  // Commit 49bab4c0046e8300c79e79b7ca9a479696c7e87a
  //
  const Expr *Init = VD->getInit();
  if (!Init)
    return false;
  if (const ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(Init))
    Init = EWC->getSubExpr();
  return isa<MaterializeTemporaryExpr>(Init);
}

/// \brief Helper class to diagnose a SIMD for loop or elemental function body.
//
/// FIXME: Check OpenMP directive and construct.
class DiagnoseSIMDForBodyHelper
  : public RecursiveASTVisitor<DiagnoseSIMDForBodyHelper> {
public:
  enum BodyKind {
    BK_SIMDFor,
    BK_Elemental
  };
private:
  Sema &SemaRef;
  /// \brief The current simd for loop location.
  SourceLocation LoopLoc;
  bool &IsValid;
  BodyKind Kind;

  void Diag(SourceLocation L, const char *Msg, SourceRange Range) {
    SemaRef.Diag(L, SemaRef.PDiag(diag::err_simd_for_body_no_construct)
                      << Msg << Kind << Range);
    IsValid = true;
  }

public:
  DiagnoseSIMDForBodyHelper(Sema &S, bool &IsValid, SourceLocation Loc,
                            enum BodyKind K)
    : SemaRef(S), LoopLoc(Loc), IsValid(IsValid), Kind(K) { }

  bool VisitGotoStmt(GotoStmt *S) {
    Diag(S->getLocStart(), "goto", S->getSourceRange());
    return true;
  }

  bool VisitIndirectGotoStmt(IndirectGotoStmt *S) {
    Diag(S->getLocStart(), "indirect goto", S->getSourceRange());
    return true;
  }

  bool VisitCilkSpawnDecl(CilkSpawnDecl *D) {
    TraverseStmt(D->getSpawnStmt());
    return true;
  }

  bool VisitCallExpr(CallExpr *E) {
    if (E->isCilkSpawnCall())
      Diag(E->getCilkSpawnLoc(), "_Cilk_spawn", E->getSourceRange());
    else if (FunctionDecl *FD = E->getDirectCallee()) {
      IdentifierInfo *II = FD->getIdentifier();
      if (!II)
        return true;

      StringRef Name = II->getName();
      if (Name.equals("setjmp") ||
          Name.equals("_setjmp") ||
          Name.equals("__builtin_setjmp") ||
          Name.equals("longjmp") ||
          Name.equals("_longjmp") ||
          Name.equals("__builtin_longjmp")) {
#if 0
        // Cannot find a realiable way to check if this call is a setjmp
        // or longjmp call. Although the spec declares this being ill-formed,
        // a warning would be more appropriate.
        SemaRef.Diag(E->getLocStart(),
          SemaRef.PDiag(diag::warn_simd_for_setjmp_longjmp)
          << Name.data() << E->getSourceRange());
#else
        Diag(E->getLocStart(), Name.data(), E->getSourceRange());
#endif
      }
    }
    return true;
  }

  bool VisitCilkSyncStmt(CilkSyncStmt *S) {
    Diag(S->getLocStart(), "_Cilk_sync", S->getSourceRange());
    return true;
  }

  bool VisitCilkForStmt(CilkForStmt *S) {
    Diag(S->getLocStart(), "_Cilk_for", S->getSourceRange());
    return true;
  }

  bool VisitCXXTryStmt(CXXTryStmt *S) {
    Diag(S->getLocStart(), "try", S->getSourceRange());
    return true;
  }

  bool VisitCXXThrowExpr(CXXThrowExpr *E) {
    Diag(E->getLocStart(), "throw", E->getSourceRange());
    return true;
  }

  // Check if there is a declaration of a non-static variable of a type with
  // a (non-trivial) destructor.
  //
  bool VisitDeclStmt(DeclStmt *S) {
    for (DeclStmt::decl_iterator I = S->decl_begin(), E = S->decl_end();
        I != E; ++I) {
      VarDecl *VD = dyn_cast<VarDecl>(*I);

      // An extern variable should be allowed, although spec does not say so.
      if (!VD || VD->isStaticLocal() || VD->hasExternalStorage())
        continue;

      QualType T = VD->getType();

      // Do not have a type yet.
      if (T.isNull())
        continue;

      // If this is a reference that binds to a temporary, then consider its
      // non-reference type.
      if (T->isReferenceType() && extendsLifetimeOfTemporary(VD))
        T = T.getNonReferenceType();

      if (const CXXRecordDecl *RD = T->getAsCXXRecordDecl())
        if (RD->hasNonTrivialDestructor()) {
          SemaRef.Diag(VD->getLocStart(),
              SemaRef.PDiag(diag::err_simd_for_body_no_nontrivial_destructor)
              << Kind << VD->getSourceRange());
          IsValid = false;
        }
    }
    return true;
  }

  bool VisitSIMDForStmt(SIMDForStmt *S) {
    if (Kind == BK_SIMDFor) {
      SemaRef.Diag(S->getForLoc(), SemaRef.PDiag(diag::err_simd_for_nested));
      SemaRef.Diag(LoopLoc, SemaRef.PDiag(diag::note_simd_for_nested));
      IsValid = false;
    }
    return true;
  }
};
} // namespace

// The following language constructs shall not appear within the body of an
// elemental function, nor in a SIMD loop.
//
// * goto statement
// * _Cilk_spawn
// * _Cilk_for
// * OpenMP directive or construct
// * try statement
// * call to setjmp
// * declaration of a non-static variable of a type with a destructor
//
static bool CheckSIMDForBody(Sema &S, Stmt *Body, SourceLocation ForLoc) {
  bool IsValid(true);
  DiagnoseSIMDForBodyHelper D(S, IsValid, ForLoc,
                              DiagnoseSIMDForBodyHelper::BK_SIMDFor);
  D.TraverseStmt(Body);
  return IsValid;
}
static void CheckElementalFunctionBody(Sema &S, FunctionDecl *F, Stmt *Body) {
  bool IsValid(true);
  DiagnoseSIMDForBodyHelper D(S, IsValid, F->getLocation(),
                              DiagnoseSIMDForBodyHelper::BK_Elemental);
  D.TraverseStmt(Body);
}

/// Enforce restrictions on Cilk Plus elemental functions.
void Sema::DiagnoseCilkElemental(FunctionDecl *F, Stmt *Body) {
  // An elemental function must not have an exception specification.
  const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(F->getType());
  if (FPT) {
    // C++11 allows an implementation to mark a destructor noexcept implicitly.
    // We allow all functions without an exception specification or with a
    // basic noexcept aka 'noexcept(true)'.
    ExceptionSpecificationType SpecTy = FPT->getExceptionSpecType();
    if (SpecTy != EST_None && SpecTy != EST_BasicNoexcept)
      // FIXME: Is the location available of the 'throw' keyword available?
      Diag(F->getLocation(), diag::err_cilk_elemental_exception_spec);
  }
  CheckElementalFunctionBody(*this, F, Body);
}

static
bool CommonActOnForStmt(Sema &S,
                        SourceLocation ForLoc,
                        SourceLocation LParenLoc,
                        Stmt *First, Expr *Cond, Expr *Increment,
                        SourceLocation RParenLoc,
                        Stmt *Body,
                        ExprResult &LoopCount,
                        Expr *&StrideExpr,
                        ExprResult &Span,
                        bool IsCilkFor) {
  assert(First && "expected init");
  assert(Cond && "expected cond");
  assert(Increment && "expected increment");

  // Check loop initializer and get control variable and its initialization
  VarDecl *ControlVar = 0;
  Expr *ControlVarInit = 0;
  if (!CheckForInit(S, First, ControlVar, ControlVarInit, IsCilkFor))
    return false;

  // Check loop condition
  if (IsCilkFor)
    S.CheckForLoopConditionalStatement(Cond, Increment, Body);

  Expr *Limit = 0;
  int CondDirection = 0;
  BinaryOperatorKind Opcode;
  CheckForCondition(S, ControlVar, Cond, Limit, CondDirection, Opcode,
                    IsCilkFor);
  if (!Limit)
    return false;

  // Remove any implicit AST node introduced by semantic analysis.
  Limit = Limit->getSubExprAsWritten();

  if (!CheckForIncrement(S, Increment, Limit, ControlVar,
                         ControlVarInit, CondDirection, StrideExpr,
                         IsCilkFor))
    return false;

  if (!IsCilkFor && !CheckSIMDForBody(S, Body, ForLoc))
    return false;

  if (!S.CurContext->isDependentContext()) {
    StrideExpr = StrideExpr->getSubExprAsWritten();

    // Push an evaluation context in case span needs cleanups.
    EnterExpressionEvaluationContext EvalContext(S, S.PotentiallyEvaluated);

    // Build end - begin
    Expr *Begin = S.BuildDeclRefExpr(ControlVar,
                                     ControlVar->getType().getNonReferenceType(),
                                     VK_LValue,
                                     ControlVar->getLocation()).release();
    Expr *End = Limit;
    if (CondDirection < 0)
      std::swap(Begin, End);

    Span = S.BuildBinOp(S.getCurScope(), ForLoc, BO_Sub, End, Begin);

    if (Span.isInvalid()) {
      // error getting operator-()
      S.Diag(ForLoc, diag::err_cilk_for_difference_ill_formed);
      S.Diag(Begin->getLocStart(), diag::note_cilk_for_begin_expr)
        << Begin->getSourceRange();
      S.Diag(End->getLocStart(), diag::note_cilk_for_end_expr)
        << End->getSourceRange();
      return false;
    }

    if (!Span.get()->getType()->isIntegralOrEnumerationType()) {
      // non-integral type
      S.Diag(ForLoc, diag::err_non_integral_cilk_for_difference_type)
        << Span.get()->getType();
      S.Diag(Begin->getLocStart(), diag::note_cilk_for_begin_expr)
        << Begin->getSourceRange();
      S.Diag(End->getLocStart(), diag::note_cilk_for_end_expr)
        << End->getSourceRange();
      return false;
    }

    assert(Span.get() && "missing span for cilk for loop count");
    LoopCount = S.CalculateCilkForLoopCount(ForLoc, Span.get(), Increment,
                                            StrideExpr, CondDirection, Opcode);

    // The loop count calculation may require cleanups in three cases:
    // a) building the binary operator- requires a cleanup
    // b) the Limit expression contains a temporary with a destructor
    // c) the Stride expression contains a temporary with a destructor
    //
    // The case (a) is handled above in BuildBinOp, but (b,c) must be checked
    // explicitly, since Limit and Stride were built in a different evaluation
    // context.
    S.ExprNeedsCleanups |= Limit->hasNonTrivialCall(S.Context);
    S.ExprNeedsCleanups |= StrideExpr->hasNonTrivialCall(S.Context);

    LoopCount = S.MakeFullExpr(LoopCount.get()).release();
  }

  S.DiagnoseUnusedExprResult(First);
  S.DiagnoseUnusedExprResult(Increment);
  S.DiagnoseUnusedExprResult(Body);
  if (isa<NullStmt>(Body)) {
    if (IsCilkFor)
      S.getCurCompoundScopeSkipCilkFor().setHasEmptyLoopBodies();
    else
      S.getCurCompoundScopeSkipSIMDFor().setHasEmptyLoopBodies();
  }

  return true;
}

StmtResult
Sema::ActOnCilkForStmt(SourceLocation CilkForLoc, SourceLocation LParenLoc,
                       Stmt *First, FullExprArg Second, FullExprArg Third,
                       SourceLocation RParenLoc, Stmt *Body) {
  ExprResult LoopCount, Span;
  Expr *StrideExpr = 0;
  if (!CommonActOnForStmt(*this, CilkForLoc, LParenLoc,
                          First, Second.get(), Third.get(),
                          RParenLoc, Body,
                          LoopCount, StrideExpr, Span,
                          /* IsCilkFor */ true))
    return StmtError();
  return BuildCilkForStmt(CilkForLoc, LParenLoc, First, Second.get(),
                          Third.get(), RParenLoc, Body, LoopCount.get(),
                          StrideExpr, Span.isUsable() ? Span.get()->getType()
                                                      : QualType());
}

StmtResult
Sema::ActOnSIMDForStmt(SourceLocation PragmaLoc,
                       ArrayRef<Attr *> Attrs,
                       SourceLocation ForLoc,
                       SourceLocation LParenLoc,
                       Stmt *First, FullExprArg Second, FullExprArg Third,
                       SourceLocation RParenLoc,
                       Stmt *Body) {
  ExprResult LoopCount, Span;
  Expr *StrideExpr = 0;
  if (!CommonActOnForStmt(*this, ForLoc, LParenLoc,
                          First, Second.get(), Third.get(),
                          RParenLoc, Body,
                          LoopCount, StrideExpr, Span,
                          /* IsCilkFor */ false))
    return StmtError();

  // The Loop Control Variable may not be the subject of a SIMD clause.
  // SIMD variables capture a local copy inside of the loop, and all
  // modifications are done on that capture. In the case that the LCV is the
  // subject of a SIMD clause, the LCV itself will not be updated.
  {
    SIMDForScopeInfo *FSI = getCurSIMDFor();
    VarDecl *LCV = const_cast<VarDecl *>(getLoopControlVariable(*this, First));
    if (FSI->IsSIMDVariable(LCV)) {
      if (FSI->isReduction(LCV) || FSI->isFirstPrivate(LCV)) {
        Diag(FSI->GetLocation(LCV), diag::err_simd_for_lcv_invalid_clause)
          << FSI->isFirstPrivate(LCV);
        return StmtError();
      }
      Diag(FSI->GetLocation(LCV), diag::warn_simd_for_variable_lcv);
      FSI->SetInvalid(LCV);
    }
  }

  return BuildSIMDForStmt(PragmaLoc, Attrs, ForLoc, LParenLoc, First,
                          Second.get(), Third.get(), RParenLoc, Body,
                          LoopCount.get());
}


AttrResult Sema::ActOnPragmaSIMDLength(SourceLocation VectorLengthLoc,
                                       Expr *VectorLengthExpr) {
  /* Perform checks that Expr is a constant, power of 2 */
  if (VectorLengthExpr->getType().isNull())
    return AttrError();

  ExprResult E;
  if (!VectorLengthExpr->isInstantiationDependent()) {
    llvm::APSInt Constant;
    SourceLocation BadConstantLoc;
    Constant.setIsUnsigned(true);
    if (!VectorLengthExpr->isIntegerConstantExpr(Constant, Context,
                                                 &BadConstantLoc)) {
      Diag(BadConstantLoc, diag::err_invalid_vectorlength_expr) << 0;
      return AttrError();
    }
    if (!Constant.isPowerOf2()) {
      Diag(VectorLengthExpr->getLocStart(),
           diag::err_invalid_vectorlength_expr) << 1;
      return AttrError();
    }
    E = Owned(IntegerLiteral::Create(
        Context, Constant, Context.getCanonicalType(VectorLengthExpr->getType()),
        VectorLengthExpr->getLocStart()));
    if (E.isInvalid())
      return AttrError();
  } else {
    // If the vector length expr is instantiation dependent, store the
    // templated expr to be transofrmed later.
    E = VectorLengthExpr;
  }

  return AttrResult(::new (Context)
                    SIMDLengthAttr(VectorLengthLoc, Context, E.get()));
}

AttrResult Sema::ActOnPragmaSIMDLengthFor(SourceLocation VectorLengthForLoc,
                                          SourceLocation TypeLoc,
                                          QualType &VectorLengthForType) {
  if (VectorLengthForType.isNull())
    return AttrError();

  QualType Ty = VectorLengthForType.getCanonicalType();
  if (Ty->isVoidType()) {
    Diag(TypeLoc, diag::err_simd_for_length_for_void);
    return AttrError();
  }
  if (!VectorLengthForType->isInstantiationDependentType() &&
      Ty->isIncompleteType()) {
    Diag(TypeLoc, diag::err_simd_for_length_for_incomplete) << Ty;
    return AttrError();
  }

  return AttrResult(::new (Context) SIMDLengthForAttr(
      VectorLengthForLoc, Context, VectorLengthForType, TypeLoc));
}

AttrResult Sema::ActOnPragmaSIMDLinear(SourceLocation LinearLoc,
                                       ArrayRef<Expr *> Exprs) {
  for (unsigned i = 0, e = Exprs.size(); i < e; i += 2) {
    if (Expr *E = Exprs[i]) {
      if (!E->isTypeDependent() && !E->getType()->isScalarType()) {
        Diag(E->getExprLoc(), diag::err_pragma_simd_invalid_linear_var);
        return AttrError();
      }
    }

    // linear-step must be either an integer constant expression,
    // or be a reference to a variable with integral type.
    if (Expr *Step = Exprs[i+1]) {
      llvm::APSInt Constant;
      if (!Step->isInstantiationDependent() && !Step->EvaluateAsInt(Constant, Context)) {
        if (!isa<DeclRefExpr>(Step)) {
          Diag(Step->getLocStart(), diag::err_pragma_simd_invalid_linear_step);
          return AttrError();
        }
        QualType SQT = Step->getType();
        if (!SQT->isIntegralType(Context)) {
          Diag(Step->getLocStart(), diag::err_pragma_simd_invalid_linear_step);
          return AttrError();
        }
      }
      if (!Constant.getBoolValue())
        Diag(Step->getLocStart(), diag::warn_pragma_simd_linear_expr_zero);
    }
  }
  return AttrResult(::new (Context) SIMDLinearAttr(
      LinearLoc, Context, const_cast<Expr **>(Exprs.data()), Exprs.size()));
}

ExprResult Sema::ActOnPragmaSIMDPrivateVariable(CXXScopeSpec SS,
                                                DeclarationNameInfo Name,
                                                SIMDPrivateKind Kind) {
  SourceLocation NameLoc = Name.getLoc();
  StringRef KindName = (Kind == SIMD_Private) ? "private" :
                   (Kind == SIMD_LastPrivate) ? "lastprivate" : "firstprivate";

  LookupResult Lookup(*this, Name, LookupOrdinaryName);
  LookupParsedName(Lookup, CurScope, &SS, /*AllowBuiltinCreation*/false);
  if (Lookup.isAmbiguous())
    return ExprError();

  if (Lookup.empty()) {
    Diag(NameLoc, diag::err_undeclared_var_use) << Name.getName();
    return ExprError();
  }

  VarDecl *VD = 0;
  if (!(VD = Lookup.getAsSingle<VarDecl>())) {
    Diag(NameLoc, diag::err_pragma_simd_invalid_private_var) << KindName;
    return ExprError();
  }

  QualType VarType = VD->getType();
  if (!VarType->isInstantiationDependentType())
    VarType = VarType.getCanonicalType();

  // A variable that appears in a {first, last} private clause must not have an
  // incomplete type or a reference type.
  if (VarType->isReferenceType()) {
    Diag(NameLoc, diag::err_pragma_simd_var_reference) << KindName << VarType;
    return ExprError();
  }

  if (!VarType->isInstantiationDependentType() && VarType->isIncompleteType()) {
    Diag(NameLoc, diag::err_pragma_simd_var_incomplete)
      << KindName << VarType;
    return ExprError();
  }

  // A variable that appears in a private or lastprivate clause must not have
  // a const-qualified type unless it is of class type with a mutable member.
  // This restriction does not apply to the firstprivate clause.
  if (VarType.isConstQualified() && (Kind != SIMD_FirstPrivate)) {
    bool HasMutableMember = false;

    if (getLangOpts().CPlusPlus && VarType->isRecordType()) {
      const RecordDecl *RD = VarType->getAs<RecordType>()->getDecl();
      assert(RD && "null RecordDecl unexpected");
      if (const CXXRecordDecl *Class = dyn_cast<CXXRecordDecl>(RD))
        HasMutableMember = Class->hasMutableFields();
    }

    if (!HasMutableMember) {
      Diag(NameLoc, diag::err_pragma_simd_var_const) << KindName;
      Diag(VD->getLocation(), diag::note_pragma_simd_var);
      return ExprError();
    }
  }

  QualType ElementTy = VarType;
  if (VarType->isArrayType())
    ElementTy = VarType->getAsArrayTypeUnsafe()->getElementType();

  CXXRecordDecl *Class = 0;
  if (getLangOpts().CPlusPlus && ElementTy->isRecordType()) {
    RecordDecl *RD = ElementTy->getAs<RecordType>()->getDecl();
    assert(RD && "null RecordDecl unexpected");
    if (isa<CXXRecordDecl>(RD))
      Class = cast<CXXRecordDecl>(RD)->getDefinition();
  }

  enum {
    DefaultConstructor = 0,
    CopyConstructor,
    CopyAssignment
  };

  // A variable of class type (or array thereof) that appears in a private
  // clause requires an accessible, unambiguous default constructor for the
  // class type.
  //
  // A variable of class type (or array thereof) that appears in a lastprivate
  // clause requires an accessible, unambiguous default constructor for the
  // class type, unless the list item is also specified in a firstprivate
  // clause.
  if (Class && (Kind == SIMD_Private || Kind == SIMD_LastPrivate)) {
    SpecialMemberOverloadResult *Result =
      LookupSpecialMember(Class, CXXDefaultConstructor, /*ConstArg=*/false,
                          /*VolatileThis=*/false, /*RValueThis*/false,
                          /*ConstThis*/false, /*VolatileThis*/false);

    switch (Result->getKind()) {
    case SpecialMemberOverloadResult::NoMemberOrDeleted:
      Diag(NameLoc, diag::err_pragma_simd_var_no_member) << KindName
        << DefaultConstructor << ElementTy;
      return ExprError();
    case SpecialMemberOverloadResult::Ambiguous:
      Diag(NameLoc, diag::err_pragma_simd_var_ambiguous_member)
        << KindName << DefaultConstructor << ElementTy;
      return ExprError();
    default:
      // Access check will be deferred until its first use.
      ;
    }
  }

  // A variable of class type (or array thereof) that appears in a firstprivate
  // clause requires an accessible, unambiguous copy constructor for the
  // class type.
  if (Class && (Kind == SIMD_FirstPrivate)) {
    SpecialMemberOverloadResult *Result =
      LookupSpecialMember(Class, CXXCopyConstructor, /*ConstArg=*/false,
                          /*VolatileThis=*/false, /*RValueThis*/false,
                          /*ConstThis*/false, /*VolatileThis*/false);

    switch (Result->getKind()) {
    case SpecialMemberOverloadResult::NoMemberOrDeleted:
      Diag(NameLoc, diag::err_pragma_simd_var_no_member) << KindName
        << CopyConstructor << ElementTy;
      return ExprError();
    case SpecialMemberOverloadResult::Ambiguous:
      Diag(NameLoc, diag::err_pragma_simd_var_ambiguous_member)
        << KindName << CopyConstructor << ElementTy;
      return ExprError();
    default:
      // Access check will be deferred until its first use.
      ;
    }
  }

  // A variable of class type (or array thereof) that appears in a lastprivate
  // clause requires an accessible, unambiguous copy assignment operator for the
  // class type.
  if (Class && (Kind == SIMD_LastPrivate)) {
    SpecialMemberOverloadResult *Result =
      LookupSpecialMember(Class, CXXCopyAssignment, /*ConstArg=*/false,
                          /*VolatileThis=*/false, /*RValueThis*/false,
                          /*ConstThis*/false, /*VolatileThis*/false);

    switch (Result->getKind()) {
    case SpecialMemberOverloadResult::NoMemberOrDeleted:
      Diag(NameLoc, diag::err_pragma_simd_var_no_member) << KindName
        << CopyAssignment << ElementTy;
      return ExprError();
    case SpecialMemberOverloadResult::Ambiguous:
      Diag(NameLoc, diag::err_pragma_simd_var_ambiguous_member)
        << KindName << CopyAssignment << ElementTy;
      return ExprError();
    default:
      // Access check will be deferred until its first use.
      ;
    }
  }

  // FIXME: Should just return this VarDecl rather than a DeclRefExpr, which
  // is a ODR-use of this declaration.
  return BuildDeclRefExpr(VD, VarType, VK_LValue, NameLoc);
}

AttrResult Sema::ActOnPragmaSIMDPrivate(SourceLocation ClauseLoc,
                                        llvm::MutableArrayRef<Expr *> Exprs,
                                        SIMDPrivateKind Kind) {
  Attr *Clause = 0;
  switch (Kind) {
  case SIMD_Private:
    Clause = ::new (Context) SIMDPrivateAttr(ClauseLoc, Context,
                                             Exprs.data(), Exprs.size());
    break;
  case SIMD_LastPrivate:
    Clause = ::new (Context) SIMDLastPrivateAttr(ClauseLoc, Context,
                                                 Exprs.data(), Exprs.size());
    break;
  case SIMD_FirstPrivate:
    Clause = ::new (Context) SIMDFirstPrivateAttr(ClauseLoc, Context,
                                                  Exprs.data(), Exprs.size());
    break;
  }

  return AttrResult(Clause);
}

AttrResult
Sema::ActOnPragmaSIMDReduction(SourceLocation ReductionLoc,
                               SIMDReductionAttr::SIMDReductionKind Operator,
                               llvm::MutableArrayRef<Expr *> VarList) {
  for (unsigned i = 0, e = VarList.size(); i < e; ++i) {
    Expr *E = VarList[i];
    const ValueDecl *VD = 0;
    if (const DeclRefExpr *D = dyn_cast<DeclRefExpr>(E)) {
      VD = D->getDecl();
    } else if (const MemberExpr *M = dyn_cast<MemberExpr>(E)) {
      VD = M->getMemberDecl();
    }

    if (!VD || VD->isFunctionOrFunctionTemplate()) {
      Diag(E->getLocStart(), diag::err_pragma_simd_reduction_invalid_var);
      return AttrError();
    }

    QualType QT = VD->getType().getCanonicalType();

    // Arrays may not appear in a reduction clause
    if (QT->isArrayType()) {
      Diag(E->getLocStart(), diag::err_pragma_simd_var_array) << "reduction";
      Diag(VD->getLocation(), diag::note_declared_at);
      return AttrError();
    }

    // An item that appears in a reduction clause must not be const-qualified.
    if (QT.isConstQualified()) {
      Diag(E->getLocStart(), diag::err_pragma_simd_var_const) << "reduction";
      Diag(VD->getLocation(), diag::note_declared_at);
      return AttrError();
    }

    BinaryOperatorKind ReductionOp;
    switch (Operator) {
    case SIMDReductionAttr::max:
    case SIMDReductionAttr::min:
      // For a max or min reduction in C/C++, the type of the list item must
      // be an allowed arithmetic data type
      if (!QT->isArithmeticType()) {
        Diag(E->getLocStart(), diag::err_pragma_simd_reduction_maxmin)
            << (Operator == SIMDReductionAttr::max);
        return AttrError();
      }
      break;
    case SIMDReductionAttr::plus:
      ReductionOp = BO_AddAssign;
      break;
    case SIMDReductionAttr::star:
      ReductionOp = BO_MulAssign;
      break;
    case SIMDReductionAttr::minus:
      ReductionOp = BO_SubAssign;
      break;
    case SIMDReductionAttr::amp:
      ReductionOp = BO_AndAssign;
      break;
    case SIMDReductionAttr::pipe:
      ReductionOp = BO_OrAssign;
      break;
    case SIMDReductionAttr::caret:
      ReductionOp = BO_XorAssign;
      break;
    case SIMDReductionAttr::ampamp:
      ReductionOp = BO_LAnd;
      break;
    case SIMDReductionAttr::pipepipe:
      ReductionOp = BO_LAnd;
      break;
    }
    if (Operator != SIMDReductionAttr::max &&
        Operator != SIMDReductionAttr::min && getLangOpts().CPlusPlus &&
        QT->isRecordType()) {
      // Test that the given operator is overloaded for this type
      if (BuildBinOp(getCurScope(), E->getLocStart(), ReductionOp, E, E)
              .isInvalid()) {
        return AttrError();
      }
    }
  }

  SIMDReductionAttr *ReductionAttr = ::new SIMDReductionAttr(
      ReductionLoc, Context, VarList.data(), VarList.size());
  ReductionAttr->Operator = Operator;

  return AttrResult(ReductionAttr);
}

namespace {
struct UsedDecl {
  // If this is allowed to conflict (eg. a linear step)
  bool CanConflict;
  // Attribute using this variable
  const Attr *Attribute;
  // Specific usage within that attribute
  const Expr *Usage;

  UsedDecl(bool CanConflict, const Attr *Attribute, const Expr *Usage)
      : CanConflict(CanConflict), Attribute(Attribute), Usage(Usage) {}
};
typedef llvm::SmallDenseMap<const ValueDecl *,
                            llvm::SmallVector<UsedDecl, 4> > DeclMapTy;

void HandleSIMDLinearAttr(const Attr *A, DeclMapTy &UsedDecls) {
  const SIMDLinearAttr *LA = static_cast<const SIMDLinearAttr *>(A);
  for (SIMDLinearAttr::linear_iterator V = LA->vars_begin(),
                                       S = LA->steps_begin(),
                                       SE = LA->steps_end();
       S != SE; ++S, ++V) {
    const Expr *LE = *V;
    if (const DeclRefExpr *D = dyn_cast_or_null<DeclRefExpr>(LE)) {
      const ValueDecl *VD = D->getDecl();
      UsedDecls[VD].push_back(UsedDecl(false, A, D));
    }
    const Expr *Step = *S;
    if (const DeclRefExpr *D = dyn_cast_or_null<DeclRefExpr>(Step)) {
      const ValueDecl *VD = D->getDecl();
      UsedDecls[VD].push_back(UsedDecl(true, LA, D));
    }
  }
}

void HandleGenericSIMDAttr(const Attr *A, DeclMapTy &UsedDecls,
                           bool CanConflict = false) {
  const SIMDReductionAttr *RA = static_cast<const SIMDReductionAttr *>(A);
  for (Expr **i = RA->variables_begin(), **e = RA->variables_end(); i < e; ++i) {
    const Expr *RE = *i;
    if (const DeclRefExpr *D = dyn_cast_or_null<DeclRefExpr>(RE)) {
      const ValueDecl *VD = D->getDecl();
      UsedDecls[VD].push_back(UsedDecl(CanConflict, A, D));
    } else if (const MemberExpr *M = dyn_cast_or_null<MemberExpr>(RE)) {
      const ValueDecl *VD = M->getMemberDecl();
      UsedDecls[VD].push_back(UsedDecl(CanConflict, A, M));
    }
  }
}

void EnforcePragmaSIMDConstraints(DeclMapTy &UsedDecls, Sema &S) {
  enum {
    Opt_Private = 0,
    Opt_LastPrivate,
    Opt_FirstPrivate,
    Opt_Linear,
    Opt_Reduction
  };

  for (DeclMapTy::iterator it = UsedDecls.begin(), end = UsedDecls.end();
       it != end; it++) {
    llvm::SmallVectorImpl<UsedDecl> &Usages = it->second;
    if (Usages.size() <= 1)
      continue;

    UsedDecl &First = Usages[0]; // Legitimate usage of this variable
    for (unsigned i = 1, e = Usages.size(); i < e; ++i) {
      UsedDecl &Use = Usages[i];
      if (First.CanConflict && Use.CanConflict)
        continue;
      // One is in conflict. Issue error on i'th usage.
      switch (Use.Attribute->getKind()) {
      case attr::SIMDPrivate:
        S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_reuse_var)
          << Opt_Private << Use.Usage->getSourceRange();
        break;
      case attr::SIMDLastPrivate:
        S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_reuse_var)
          << Opt_LastPrivate << Use.Usage->getSourceRange();
        break;
      case attr::SIMDFirstPrivate:
        S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_reuse_var)
          << Opt_FirstPrivate << Use.Usage->getSourceRange();
        break;
      case attr::SIMDLinear:
        if (Use.CanConflict || (First.CanConflict &&
                                First.Attribute->getKind() == attr::SIMDLinear))
          // This is a linear step, or in conflict with a linear step
          S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_conflict_step)
            << !Use.CanConflict << Use.Usage->getSourceRange();
        else
          // Re-using linear variable in another simd clause
          S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_reuse_var)
            << Opt_Linear << Use.Usage->getSourceRange();
        break;
      case attr::SIMDReduction:
        // Any number of reduction clauses can be specified on the directive,
        // but a list item can appear only once in the reduction clauses for
        // that directive.
        S.Diag(Use.Usage->getLocStart(), diag::err_pragma_simd_reuse_var)
          << Opt_Reduction << Use.Usage->getSourceRange();
        break;
      default:
        ;
      }
      S.Diag(First.Usage->getLocStart(), diag::note_pragma_simd_used_here)
        << First.Attribute->getRange() << First.Usage->getSourceRange();
    }
  }
}
} // namespace

void Sema::CheckSIMDPragmaClauses(SourceLocation PragmaLoc,
                                  ArrayRef<Attr *> Attrs) {
  // Collect all used decls in order of usage
  DeclMapTy UsedDecls;
  const Attr *VectorLengthAttr = 0;
  for (unsigned i = 0, e = Attrs.size(); i < e; ++i) {
    if (!Attrs[i])
      continue;
    const Attr *A = Attrs[i];

    switch(A->getKind()) {
    case attr::SIMDLinear:
      HandleSIMDLinearAttr(A, UsedDecls);
      break;
    case attr::SIMDLength:
    case attr::SIMDLengthFor:
      if (VectorLengthAttr) {
        attr::Kind ThisKind = A->getKind(),
                   PrevKind = VectorLengthAttr->getKind();

        if (ThisKind == PrevKind)
          Diag(A->getLocation(), diag::err_pragma_simd_vectorlength_multiple)
            << (ThisKind == attr::SIMDLength) << A->getRange();
        else
          Diag(A->getLocation(), diag::err_pragma_simd_vectorlength_conflict)
              << (ThisKind == attr::SIMDLength) << A->getRange();
        Diag(VectorLengthAttr->getLocation(),
             diag::note_pragma_simd_specified_here)
            << (PrevKind == attr::SIMDLength) << VectorLengthAttr->getRange();
      } else
        VectorLengthAttr = A;
      break;
    case attr::SIMDFirstPrivate:
    case attr::SIMDLastPrivate:
      HandleGenericSIMDAttr(A, UsedDecls, true);
      break;
    case attr::SIMDPrivate:
    case attr::SIMDReduction:
      HandleGenericSIMDAttr(A, UsedDecls);
      break;
    default:
      ;
    }
  }
  EnforcePragmaSIMDConstraints(UsedDecls, *this);
}

#define ADD_SIMD_VAR_LIST(ATTR, FUNC)                                          \
do {                                                                           \
  const ATTR *PA = static_cast<const ATTR *>(A);                               \
  for (Expr **I = PA->variables_begin(), **E = PA->variables_end();            \
    I != E; ++I) {                                                             \
    Expr *DE = *I;                                                             \
    assert (DE && isa<DeclRefExpr>(DE) && "reference to a variable expected"); \
    VarDecl *VD = cast<VarDecl>(cast<DeclRefExpr>(DE)->getDecl());             \
    getCurSIMDFor()->FUNC(VD, A->getLocation());                               \
  }                                                                            \
} while (0)

void Sema::ActOnStartOfSIMDForStmt(SourceLocation PragmaLoc, Scope *CurScope,
                                   ArrayRef<Attr *> SIMDAttrList) {
  CheckSIMDPragmaClauses(PragmaLoc, SIMDAttrList);

  CapturedDecl *CD = 0;
  RecordDecl *RD = CreateCapturedStmtRecordDecl(CD, PragmaLoc, /*NumArgs*/ 3);

  PushSIMDForScope(CurScope, CD, RD, PragmaLoc);

  // Process all variables in the clauses.
  for (unsigned I = 0, N = SIMDAttrList.size(); I < N; ++I) {
    const Attr *A = SIMDAttrList[I];
    if (!A)
      continue;
    switch(A->getKind()) {
    case attr::SIMDPrivate:
      ADD_SIMD_VAR_LIST(SIMDPrivateAttr, addPrivateVar);
      break;
    case attr::SIMDLastPrivate:
      ADD_SIMD_VAR_LIST(SIMDLastPrivateAttr, addLastPrivateVar);
      break;
    case attr::SIMDFirstPrivate:
      ADD_SIMD_VAR_LIST(SIMDFirstPrivateAttr, addFirstPrivateVar);
      break;
    case attr::SIMDReduction:
#if 0
      // TODO: Need to properly handle Reductions, in case of MemberExpr
      // rather than a DeclRefExpr
      ADD_SIMD_VAR_LIST(SIMDReductionAttr, addReductionVar);
#endif
      break;
    case attr::SIMDLinear: {
      const SIMDLinearAttr *LA = static_cast<const SIMDLinearAttr *>(A);
      for (SIMDLinearAttr::linear_iterator I = LA->vars_begin(),
                                           E = LA->vars_end();
           I != E; ++I) {
        Expr *DE = *I;
        assert (DE && isa<DeclRefExpr>(DE) && "reference to a variable expected");
        VarDecl *VD = cast<VarDecl>(cast<DeclRefExpr>(DE)->getDecl());
        getCurSIMDFor()->addLinearVar(VD, A->getLocation());
      }
      break;
    }
    default:
      ;
    }
  }

  // Push a compound scope for the body.
  PushCompoundScope();

  if (CurScope)
    PushDeclContext(CurScope, CD);
  else
    CurContext = CD;

  PushExpressionEvaluationContext(PotentiallyEvaluated);
}

static void ActOnForStmtError(Sema &S, RecordDecl *Record) {
  S.DiscardCleanupsInEvaluationContext();
  S.PopExpressionEvaluationContext();

  S.PopDeclContext();

  Record->setInvalidDecl();

  SmallVector<Decl*, 4> Fields;
  for (RecordDecl::field_iterator I = Record->field_begin(),
                                  E = Record->field_end(); I != E; ++I)
    Fields.push_back(*I);
  S.ActOnFields(/*Scope=*/0, Record->getLocation(), Record, Fields,
                SourceLocation(), SourceLocation(), /*AttributeList=*/0);

  // Pop the compound scope we inserted implicitly.
  S.PopCompoundScope();
  S.PopFunctionScopeInfo();
}

void Sema::ActOnCilkForStmtError() {
  ActOnForStmtError(*this, getCurCilkFor()->TheRecordDecl);
}

void Sema::ActOnSIMDForStmtError() {
  ActOnForStmtError(*this, getCurSIMDFor()->TheRecordDecl);
}

static Expr *IgnoreImplicitNodeForCilkSpawnCall(Expr *E) {
  while (true) {
    if (ImplicitCastExpr *CE = dyn_cast<ImplicitCastExpr>(E))
      E = CE->getSubExprAsWritten();
    else if (ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(E))
      E = EWC->getSubExpr();
    else if (MaterializeTemporaryExpr *MTE
        = dyn_cast<MaterializeTemporaryExpr>(E))
      E = MTE->GetTemporaryExpr();
    else if (CXXBindTemporaryExpr *BTE = dyn_cast<CXXBindTemporaryExpr>(E))
      E = BTE->getSubExpr();
    else if (CXXConstructExpr *CE = dyn_cast<CXXConstructExpr>(E)) {
      // CXXTempoaryObjectExpr represents a functional cast with != 1 arguments
      // so handle it the same way as CXXFunctionalCastExpr
      if (isa<CXXTemporaryObjectExpr>(CE))
        break;
      if (CE->getNumArgs() >= 1)
        E = CE->getArg(0);
      else
        break;
    } else
      break;
  }

  return E;
}

static bool CheckUnsupportedCall(Sema &S, CallExpr *Call) {
  assert(Call->isCilkSpawnCall() && "Cilk spawn expected");

  SourceLocation SpawnLoc = Call->getCilkSpawnLoc();
  if (Call->isBuiltinCall()) {
    S.Diag(SpawnLoc, diag::err_cannot_spawn_builtin);
    return false;
  } else if (isa<UserDefinedLiteral>(Call) || isa<CUDAKernelCallExpr>(Call)) {
    S.Diag(SpawnLoc, diag::err_cannot_spawn_function);
    return false;
  }

  return true;
}

static bool CheckSpawnCallExpr(Sema &S, Expr *E, SourceLocation SpawnLoc) {
  // Do not need to check an already checked spawn.
  if (isa<CilkSpawnExpr>(E))
    return true;

  E = IgnoreImplicitNodeForCilkSpawnCall(E);
  Expr *RHS = 0;

  // x = _Cilk_spawn f(); // x is a non-class object.
  if (BinaryOperator *B = dyn_cast<BinaryOperator>(E)) {
    if (B->getOpcode() != BO_Assign) {
      S.Diag(SpawnLoc, diag::err_spawn_not_whole_expr) << E->getSourceRange();
      return false;
    }
    RHS = B->getRHS();
  } else {
    CallExpr *Call = dyn_cast<CallExpr>(E);

    // Not a call.
    if (!Call) {
      S.Diag(SpawnLoc, diag::err_spawn_not_whole_expr) << E->getSourceRange();
      return false;
    }

    // This is a spawn call.
    if (Call->isCilkSpawnCall())
      return CheckUnsupportedCall(S, Call);

    CXXOperatorCallExpr *OC = dyn_cast<CXXOperatorCallExpr>(E);
    if (!OC || (OC->getOperator() != OO_Equal)) {
      S.Diag(SpawnLoc, diag::err_spawn_not_whole_expr) << E->getSourceRange();
      return false;
    }
    // x = _Cilk_spawn f(); // x is a class object.
    RHS = OC->getArg(1);
  }

  // Up to this point, RHS is expected to be '_Cilk_spawn f()'.
  RHS = IgnoreImplicitNodeForCilkSpawnCall(RHS);

  CallExpr *Call = dyn_cast<CallExpr>(RHS);
  if (!Call || !Call->isCilkSpawnCall()) {
    S.Diag(SpawnLoc, diag::err_spawn_not_whole_expr) << E->getSourceRange();
    return false;
  }

  return CheckUnsupportedCall(S, Call);
}

bool Sema::DiagCilkSpawnFullExpr(Expr *EE) {
  bool IsValid = true;
  assert(EE && !CilkSpawnCalls.empty() && "Cilk spawn expected");
  SourceLocation SpawnLoc = (*CilkSpawnCalls.rbegin())->getCilkSpawnLoc();
  if (CilkSpawnCalls.size() == 1) {
    // Only a single Cilk spawn in this expression; check if it is
    // in a proper place:
    //
    // (1) _Cilk_spawn f()
    //
    // (2) x = _Cilk_spawn f()
    //
    // where the second form may be "operator=" call.
    IsValid = CheckSpawnCallExpr(*this, EE, SpawnLoc);
  } else {
    SmallVectorImpl<CallExpr *>::reverse_iterator I = CilkSpawnCalls.rbegin();
    SmallVectorImpl<CallExpr *>::reverse_iterator E = CilkSpawnCalls.rend();

    IsValid = false;
    Diag(SpawnLoc, diag::err_multiple_spawns) << EE->getSourceRange();
    // Starting from the second spawn, emit a note.
    for (++I; I != E; ++I)
      Diag((*I)->getCilkSpawnLoc(), diag::note_multiple_spawns);
  }

  // FIXME: There are a number of places where invariant of a full expression
  // is not well-maitained. We clear spawn calls explicitly here.
  CilkSpawnCalls.clear();

  return IsValid;
}

namespace {

class CaptureBuilder: public RecursiveASTVisitor<CaptureBuilder> {
  Sema &S;

public:
  CaptureBuilder(Sema &S) : S(S) {}

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    S.MarkDeclRefReferenced(E);
    return true;
  }

  bool TraverseLambdaExpr(LambdaExpr *E) {
    LambdaExpr::capture_init_iterator CI = E->capture_init_begin();

    for (LambdaExpr::capture_iterator C = E->capture_begin(),
                                   CEnd = E->capture_end();
                                     C != CEnd; ++C, ++CI) {
      if (C->capturesVariable())
        S.MarkVariableReferenced((*CI)->getLocStart(), C->getCapturedVar());
      else {
        assert(C->capturesThis() && "Capturing this expected");
        assert(isa<CXXThisExpr>(*CI) && "CXXThisExpr expected");
        S.CheckCXXThisCapture((*CI)->getLocStart(), /*explicit*/false);
      }
    }
    assert(CI == E->capture_init_end() && "out of sync");
    // Only traverse the captures, and skip the body.
    return true;
  }

  /// Skip captured statements
  bool TraverseCapturedStmt(CapturedStmt *) { return true; }

  bool VisitCXXThisExpr(CXXThisExpr *E) {
    S.CheckCXXThisCapture(E->getLocStart(), /*explicit*/false);
    return true;
  }
};

void CaptureVariablesInStmt(Sema &SemaRef, Stmt *S) {
  CaptureBuilder Builder(SemaRef);
  Builder.TraverseStmt(S);
}

void MarkFunctionAsSpawning(Sema &S) {
  DeclContext *DC = S.CurContext;
  while (!DC->isFunctionOrMethod())
    DC = DC->getParent();

  Decl::Kind Kind = DC->getDeclKind();
  if (Kind >= Decl::firstFunction && Kind <= Decl::lastFunction)
    FunctionDecl::castFromDeclContext(DC)->setSpawning();
  else if (Decl::Captured == Kind)
    CapturedDecl::castFromDeclContext(DC)->setSpawning();
  else {
    S.Diag(SourceLocation(), diag::err_spawn_invalid_decl)
      << DC->getDeclKindName();
  }
}

} // namespace

ExprResult Sema::BuildCilkSpawnExpr(Expr *E) {
  if (!E)
    return ExprError();

  if (ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(E))
    E = EWC->getSubExpr();

  if (CilkSpawnExpr *CSE = dyn_cast<CilkSpawnExpr>(E)) {
    E = CSE->getSpawnExpr();
    assert(E && "Expr expected");
  }

  Stmt *Body = 0;
  {
    // Capture variables used in this full expression.
    ActOnCapturedRegionStart(E->getLocStart(), /*Scope*/0, CR_CilkSpawn,
                             /*NumParams*/1);
    CaptureVariablesInStmt(*this, E);
    Body = ActOnCapturedRegionEnd(E).get();
  }

  DeclContext *DC = CurContext;
  while (!(DC->isFunctionOrMethod() || DC->isRecord() || DC->isFileContext()))
    DC = DC->getParent();

  CapturedStmt *CS = cast<CapturedStmt>(Body);
  CilkSpawnDecl *Spawn = CilkSpawnDecl::Create(Context, DC, CS);
  DC->addDecl(Spawn);

  MarkFunctionAsSpawning(*this);
  return Owned(new (Context) CilkSpawnExpr(Spawn, E->getType()));
}

static QualType GetReceiverTmpType(const Expr *E) {
  do {
    if (const ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(E))
      E = EWC->getSubExpr();
    const MaterializeTemporaryExpr *M = NULL;
    E = E->findMaterializedTemporary(M);
  } while (isa<ExprWithCleanups>(E));

  // Skip any implicit casts.
  SmallVector<const Expr *, 2> CommaLHSs;
  SmallVector<SubobjectAdjustment, 2> Adjustments;
  E = E->skipRValueSubobjectAdjustments(CommaLHSs, Adjustments);

  return E->getType();
}

static void addReceiverParams(Sema &SemaRef, CapturedDecl *CD,
                              QualType ReceiverType,
                              QualType ReceiverTmpType) {
  if (!ReceiverType.isNull()) {
    DeclContext *DC = CapturedDecl::castToDeclContext(CD);
    assert(CD->getNumParams() >= 2);
    ImplicitParamDecl *Receiver
      = ImplicitParamDecl::Create(SemaRef.getASTContext(), DC, SourceLocation(),
                                  /*IdInfo*/0, ReceiverType);
    DC->addDecl(Receiver);
    CD->setParam(1, Receiver);
    if (!ReceiverTmpType.isNull()) {
      assert(CD->getNumParams() == 3);
      ImplicitParamDecl *ReceiverTmp
        = ImplicitParamDecl::Create(SemaRef.getASTContext(), DC,
                                    SourceLocation(), /*IdInfo*/0,
                                    ReceiverTmpType);
      DC->addDecl(ReceiverTmp);
      CD->setParam(2, ReceiverTmp);
    }
  }
}

CilkSpawnDecl *Sema::BuildCilkSpawnDecl(Decl *D) {
  VarDecl *VD = dyn_cast_or_null<VarDecl>(D);
  if (!VD || VD->isInvalidDecl())
    return 0;

  assert(VD->hasInit() && "initializer expected");

  if (VD->isStaticLocal()) {
    Diag(VD->getLocation(), diag::err_cannot_init_static_variable)
      << VD->getSourceRange();
    return 0;
  }

  // Pass the receiver (and possibly receiver temporary) to the
  // captured statement.
  unsigned NumParams = 2;

  // Receiver.
  QualType ReceiverType = Context.getCanonicalType(VD->getType());
  ReceiverType = Context.getPointerType(ReceiverType);

  // Receiver temporary.
  QualType ReceiverTmpType;
  if (VD->getType()->isReferenceType() && extendsLifetimeOfTemporary(VD)) {
    ReceiverTmpType = GetReceiverTmpType(VD->getInit());
    if (!ReceiverTmpType.isNull()) {
      NumParams = 3;
      ReceiverTmpType = Context.getPointerType(ReceiverTmpType);
    }
  }

  // Start building the Captured statement.
  ActOnCapturedRegionStart(D->getLocStart(), /*Scope*/0, CR_CilkSpawn,
                           NumParams);

  // Create a DeclStmt as the CapturedStatement body.
  DeclStmt *Body = new (Context) DeclStmt(DeclGroupRef(VD),
                                          VD->getLocStart(),
                                          VD->getLocEnd());
  CaptureVariablesInStmt(*this, Body);

  StmtResult R = ActOnCapturedRegionEnd(Body);

  DeclContext *DC = CurContext;
  while (!(DC->isFunctionOrMethod() || DC->isRecord() || DC->isFileContext()))
    DC = DC->getParent();

  CapturedStmt *CS = cast<CapturedStmt>(R.release());
  CilkSpawnDecl *Spawn = CilkSpawnDecl::Create(Context, DC, CS);
  DC->addDecl(Spawn);

  // Initialize receiver and its associated temporary parameters.
  addReceiverParams(*this, Spawn->getCapturedStmt()->getCapturedDecl(),
                    ReceiverType, ReceiverTmpType);

  MarkFunctionAsSpawning(*this);
  return Spawn;
}

namespace {
// Diagnose any _Cilk_spawn expressions (see comment below). InSpawn indicates
// that S is contained within a spawn, e.g. _Cilk_spawn foo(_Cilk_spawn bar())
class DiagnoseCilkSpawnHelper
  : public RecursiveASTVisitor<DiagnoseCilkSpawnHelper> {
  Sema &SemaRef;
public:
  explicit DiagnoseCilkSpawnHelper(Sema &S) : SemaRef(S) { }

  bool TraverseCompoundStmt(CompoundStmt *) { return true; }
  bool VisitCallExpr(CallExpr *E) {
    if (E->isCilkSpawnCall())
      SemaRef.Diag(E->getCilkSpawnLoc(),
          SemaRef.PDiag(diag::err_spawn_not_whole_expr) << E->getSourceRange());
    return true;
  }
  bool VisitCilkSpawnDecl(CilkSpawnDecl *D) {
    TraverseStmt(D->getSpawnStmt());
    return true;
  }
};

} // anonymous namespace

// Check that _Cilk_spawn is used only:
//  - as the entire body of an expression statement,
//  - as the entire right hand side of an assignment expression that is the
//    entire body of an expression statement, or
//  - as the entire initializer-clause in a simple declaration.
//
// Since this is run per-compound scope stmt, we don't traverse into sub-
// compound scopes, but we do need to traverse into loops, ifs, etc. in case of:
// if (cond) _Cilk_spawn foo();
//           ^~~~~~~~~~~~~~~~~ not a compound scope
void Sema::DiagnoseCilkSpawn(Stmt *S) {
  DiagnoseCilkSpawnHelper D(*this);

  // already checked.
  if (isa<Expr>(S))
    return;

  switch (S->getStmtClass()) {
  case Stmt::AttributedStmtClass:
    DiagnoseCilkSpawn(cast<AttributedStmt>(S)->getSubStmt());
    break;
  case Stmt::CompoundStmtClass:
  case Stmt::DeclStmtClass:
    return; // already checked
  case Stmt::CapturedStmtClass:
    DiagnoseCilkSpawn(cast<CapturedStmt>(S)->getCapturedStmt());
    break;
  case Stmt::CilkForStmtClass: {
    CilkForStmt *CF = cast<CilkForStmt>(S);
    if (CF->getInit())
      D.TraverseStmt(CF->getInit());
    if (CF->getCond())
      D.TraverseStmt(CF->getCond());
    if (CF->getInc())
      D.TraverseStmt(CF->getInc());
    // the cilk for body is already checked.
    break;
  }
  case Stmt::CXXCatchStmtClass:
    DiagnoseCilkSpawn(cast<CXXCatchStmt>(S)->getHandlerBlock());
    break;
  case Stmt::CXXForRangeStmtClass: {
    CXXForRangeStmt *FR = cast<CXXForRangeStmt>(S);
    D.TraverseStmt(FR->getRangeInit());
    DiagnoseCilkSpawn(FR->getBody());
    break;
  }
  case Stmt::CXXTryStmtClass:
    DiagnoseCilkSpawn(cast<CXXTryStmt>(S)->getTryBlock());
    break;
  case Stmt::DoStmtClass: {
    DoStmt *DS = cast<DoStmt>(S);
    D.TraverseStmt(DS->getCond());
    DiagnoseCilkSpawn(DS->getBody());
    break;
  }
  case Stmt::ForStmtClass: {
    ForStmt *F = cast<ForStmt>(S);
    if (F->getInit())
      D.TraverseStmt(F->getInit());
    if (F->getCond())
      D.TraverseStmt(F->getCond());
    if (F->getInc())
      D.TraverseStmt(F->getInc());
    DiagnoseCilkSpawn(F->getBody());
    break;
  }
  case Stmt::IfStmtClass: {
    IfStmt *I = cast<IfStmt>(S);
    D.TraverseStmt(I->getCond());
    DiagnoseCilkSpawn(I->getThen());
    if (I->getElse())
      DiagnoseCilkSpawn(I->getElse());
    break;
  }
  case Stmt::LabelStmtClass:
    DiagnoseCilkSpawn(cast<LabelStmt>(S)->getSubStmt());
    break;
  case Stmt::SwitchStmtClass: {
    SwitchStmt *SS = cast<SwitchStmt>(S);
    if (const DeclStmt *DS = SS->getConditionVariableDeclStmt())
      D.TraverseStmt(const_cast<DeclStmt *>(DS));
    D.TraverseStmt(SS->getCond());
    DiagnoseCilkSpawn(SS->getBody());
    break;
  }
  case Stmt::CaseStmtClass:
  case Stmt::DefaultStmtClass:
    DiagnoseCilkSpawn(cast<SwitchCase>(S)->getSubStmt());
    break;
  case Stmt::WhileStmtClass: {
    WhileStmt *W = cast<WhileStmt>(S);
    D.TraverseStmt(W->getCond());
    DiagnoseCilkSpawn(W->getBody());
    break;
  }
  default:
    D.TraverseStmt(S);
    break;
  }
}

bool Sema::DiagnoseElementalAttributes(FunctionDecl *FD) {
  // Cache the function parameter with names.
  llvm::SmallDenseMap<IdentifierInfo *, const ParmVarDecl *, 8> Params;
  for (FunctionDecl::param_iterator I = FD->param_begin(), E = FD->param_end();
      I != E; ++I)
    if (IdentifierInfo *II = (*I)->getIdentifier())
      Params[II] = *I;

  // Group elemental function attributes by vector() declaration.
  typedef llvm::SmallVector<Attr *, 4> AttrVec;
  typedef llvm::SmallDenseMap<unsigned, AttrVec, 4> GroupMap;

  GroupMap Groups;
  for (Decl::attr_iterator AI = FD->attr_begin(), AE = FD->attr_end();
       AI != AE; ++AI) {
    if (CilkElementalAttr *A = dyn_cast<CilkElementalAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups.FindAndConstruct(key);
    } else if (CilkProcessorAttr *A = dyn_cast<CilkProcessorAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    } else if (CilkVecLengthForAttr *A = dyn_cast<CilkVecLengthForAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    } else if (CilkVecLengthAttr *A = dyn_cast<CilkVecLengthAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    } else if (CilkMaskAttr *A = dyn_cast<CilkMaskAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    } else if (CilkLinearAttr *A = dyn_cast<CilkLinearAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    } else if (CilkUniformAttr *A = dyn_cast<CilkUniformAttr>(*AI)) {
      unsigned key = A->getGroup().getRawEncoding();
      Groups[key].push_back(A);
    }
  }

  // Within an attribute group, check the following:
  //
  // (1) No parameter of an elemental function shall be the subject of more
  //     than one uniform or linear clause;
  //
  // (2) An elemental linear variable shall have integeral or pointer type;
  //
  // (3) A parameter referenced as an elemental linear step shall be the
  //     subject of a uniform clause.
  //
  // (4) A vector attribute shall not have both a mask clause and a
  //     nomask clause.
  //
  bool Valid = true;
  for (GroupMap::iterator GI = Groups.begin(), GE = Groups.end();
       GI != GE; ++GI) {
    AttrVec &Attrs = GI->second;

    typedef llvm::SmallDenseMap<IdentifierInfo *, Attr *> SubjectAttrMapTy;
    SubjectAttrMapTy SubjectNames;
    CilkMaskAttr *PrevMaskAttr = 0;

    // Check (1).
    for (AttrVec::iterator I = Attrs.begin(), E = Attrs.end(); I != E; ++I) {
      IdentifierInfo *II = 0;
      SourceLocation SubjectLoc;

      Attr *CurA = *I;
      if (CilkUniformAttr *UA = dyn_cast<CilkUniformAttr>(CurA)) {
        II = UA->getParameter();
        SubjectLoc = UA->getParameterLoc();
      } else if (CilkLinearAttr *LA = dyn_cast<CilkLinearAttr>(CurA)) {
        II = LA->getParameter();
        SubjectLoc = LA->getParameterLoc();
      } else if (CilkMaskAttr *MA = dyn_cast<CilkMaskAttr>(CurA)) {
        // Check (4)
        if (PrevMaskAttr && PrevMaskAttr->getMask() != MA->getMask()) {
          Diag(MA->getLocation(), diag::err_cilk_elemental_both_mask_nomask)
            << MA->getRange();
          Diag(PrevMaskAttr->getLocation(),
               diag::note_cilk_elemental_mask_here)
            << PrevMaskAttr->getMask() << PrevMaskAttr->getRange();
          Valid = false;
        } else
          PrevMaskAttr = MA;
        continue;
      } else
        continue;

      // If this is the subject of a linear or uniform attribute, check if
      // this is already the subject of a previous linear or uniform attribute.
      if (Attr *PrevA = SubjectNames.lookup(II)) {
        Valid = false;

        const ParmVarDecl *VD = Params[II];
        if (!VD) {
          // If II is not a parameter name, then it is "this".
          assert(getLangOpts().CPlusPlus && II->getName().equals("this")
                                         && "invalid attribute");
          Diag(SubjectLoc, diag::err_cilk_elemental_this_subject);
        } else
          Diag(SubjectLoc, diag::err_cilk_elemental_subject) << VD;

        if (CilkUniformAttr *UA = dyn_cast<CilkUniformAttr>(PrevA))
          Diag(UA->getParameterLoc(), diag::note_cilk_elemental_subject_clause)
            << 0;
        else if (CilkLinearAttr *LA = dyn_cast<CilkLinearAttr>(PrevA))
          Diag(LA->getParameterLoc(), diag::note_cilk_elemental_subject_clause)
            << 1;

        // Only emit a note if II is a parameter name.
        if (VD)
          Diag(VD->getLocation(), diag::note_cilk_elemental_subject_parameter);
       } else
         SubjectNames[II] = CurA;
    }

    for (SubjectAttrMapTy::iterator I = SubjectNames.begin(),
                                    E = SubjectNames.end(); I != E; ++I) {
      Attr *CurA = I->second;
      if (CilkLinearAttr *LA = dyn_cast<CilkLinearAttr>(CurA)) {
        // Check (2).
        //
        // No need to check 'linear(this)'. Only check if the subject is an
        // explicit parameter name.
        //
        IdentifierInfo *Subject = LA->getParameter();
        if (const ParmVarDecl *Param = Params.lookup(Subject)) {
          QualType Ty = Param->getType();
          if (!Ty->isDependentType() && !Ty->isIntegralType(Context)
                                     && !Ty->isPointerType()) {
            Diag(LA->getParameterLoc(),
                 diag::err_cilk_elemental_linear_parameter_type) << Ty;
            Valid = false;
          }
        }

        // Check (3)
        if (IdentifierInfo *Step = LA->getStepParameter()) {
          // If this is not a parameter name, then skip this check since
          // it must be a compile time constant.
          if (!Params.count(Step))
            continue;

          // Otherwise, this is referencing a parameter name, which shall be
          // the subject of a uniform clause in this group.
          Attr *A = SubjectNames.lookup(Step);
          if (!A || !isa<CilkUniformAttr>(A)) {
            Diag(LA->getStepLoc(), diag::err_cilk_elemental_step_not_uniform);
            Valid = false;
          }
        }
      }
    }
  }

  return Valid;
}

Expr *Sema::CheckCilkVecLengthArg(Expr *E) {
  if (!E)
    return E;

  SourceLocation ExprLoc = E->getExprLoc();
  QualType Ty = E->getType().getNonReferenceType();

  // Check type.
  if (!E->isTypeDependent()) {
    if (!Ty->isIntegralOrEnumerationType()) {
      Diag(ExprLoc, diag::err_attribute_argument_type)
        << "vectorlength" << AANT_ArgumentIntegerConstant << E->getSourceRange();
      return 0;
    }
  }

  // Check value if it is not inside a template.
  if (!E->isInstantiationDependent()) {
    llvm::APSInt Val(Context.getTypeSize(Ty), /*isUnsigned*/false);
    SourceLocation BadExprLoc;

    if (!E->isIntegerConstantExpr(Val, Context, &BadExprLoc)) {
      Diag(BadExprLoc, diag::err_attribute_argument_type)
        << "vectorlength" << AANT_ArgumentIntegerConstant << E->getSourceRange();
      return 0;
    }

    if (!Val.isStrictlyPositive()) {
      Diag(ExprLoc, diag::err_cilk_elemental_vectorlength)
        << E->getSourceRange();
      return 0;
    }

    if (!Val.isPowerOf2()) {
      Diag(ExprLoc, diag::err_invalid_vectorlength_expr)
        << 1 << E->getSourceRange();
      return 0;
    }

    // Create an integeral literal for the final attribute.
    return IntegerLiteral::Create(Context, Val, Ty, ExprLoc);
  }

  // Cannot evaluate this expression yet.
  return E;
}

Expr *Sema::CheckCilkLinearArg(Expr *E) {
  if (!E)
    return E;

  SourceLocation ExprLoc = E->getExprLoc();
  QualType Ty = E->getType().getNonReferenceType();

  // Check type.
  if (!E->isTypeDependent() && !Ty->isIntegralOrEnumerationType()) {
    Diag(ExprLoc, diag::err_cilk_elemental_linear_step_not_integral)
      << E->getType() << E->getSourceRange();
    return 0;
  }

  if (!E->isInstantiationDependent()) {
    // If this is a parameter name, then return itself. Otherwise, make
    // sure it is an integer constant.
    if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
      if (isa<ParmVarDecl>(DRE->getDecl()))
        return E;

    llvm::APSInt Val(Context.getTypeSize(Ty), /*isUnsigned*/false);
    if (!E->isIntegerConstantExpr(Val, Context)) {
      Diag(ExprLoc, diag::err_cilk_elemental_linear_step_not_constant)
        << E->getSourceRange();
      return 0;
    }

    // Create an integeral literal for the final attribute.
    return IntegerLiteral::Create(Context, Val, Ty, ExprLoc);
  }

  return E;
}
