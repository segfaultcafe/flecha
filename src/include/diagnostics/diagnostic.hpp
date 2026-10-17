#ifndef FLECHA_DIAGNOSTICS_DIAGNOSTIC_HPP
#define FLECHA_DIAGNOSTICS_DIAGNOSTIC_HPP

#include <string>
#include <string_view>

#include "source/source_range.hpp"

namespace flecha::diagnostics {

enum class DiagnosticSeverity { Note, Warning, Error };

enum class DiagnosticCode {
    UnexpectedCharacter,
    UnterminatedBlockComment,
    UnterminatedStringLiteral,
    InvalidStringEscape,
    InvalidCharacterLiteral,
    InvalidCharacterEscape,
    UnexpectedToken,
    ExpectedToken,
    InvalidAssignmentTarget,
    DuplicateDeclaration,
    UnknownName,
    IncompatibleType,
    SemanticInvalidAssignmentTarget,
    SymbolNotWritable,
    NotCallable,
    WrongArgumentCount,
    IncompatibleArgument,
    InvalidOperand,
    InvalidCondition,
    InvalidReturnType,
    CannotInferType,
    InvalidVariableType,
    LoopControlOutsideLoop,
    DuplicatePosition,
    UnknownPosition,
    InvalidCast,
    InvalidMember,
    NotIndexable,
    BorrowEscapesOwner,
    UninitializedVariable,
    BorrowRequiresInitializer,
    ConstRequiresInitializer,
    UnsupportedCompoundType,
    UnsafeOperation,
    InvalidPointerAccess,
    ReferenceRequiresInitializer,
    MovedReference,
    BorrowConflict,
    ReferenceEscape,
    InvalidReference,
    IntegerLiteralOutOfRange,
    InvalidArrayLength,
    InvalidArrayElement,
    ArrayInitializerCount,
    ArrayIndexOutOfBounds,
    SliceBounds,
    InvalidDefer,
    InvalidTuple,
    InvalidDestructure,
    InvalidErrorDeclaration,
    UnknownType,
    InvalidStructure,
    PrivateAccess,
    InvalidLifecycle,
    NonCopyableValue,
    InvalidGeneric,
    InvalidAllocation,
    MemoryLeak,
    DoubleFree,
    UseAfterFree,
    InvalidFree,
    UnverifiedOwnership,
    NullDereference,
    InvalidExtern,
    InvalidEnumDeclaration,
    InvalidMatch,
    InvalidBorrowContract,
    MovedValue,
    NativeTargetUnavailable,
    ObjectEmissionFailed,
    RuntimeUnavailable,
    LinkerFailed,
};

[[nodiscard]] std::string_view DiagnosticCodeName(DiagnosticCode code);

struct Diagnostic {
    DiagnosticCode code;
    DiagnosticSeverity severity;
    std::string message;
    source::SourceRange range;
};

}  // namespace flecha::diagnostics

#endif  // FLECHA_DIAGNOSTICS_DIAGNOSTIC_HPP
