// Flecha Programming Language
// Copyright (c) 2024 J. A. C. Buschinelli - Flecha
// This code is licensed under the MIT License. See LICENSE for details.

#ifndef FLECHA_TOKENTYPE_HPP
#define FLECHA_TOKENTYPE_HPP

enum class TokenType {
    // Primitive types
    I8,
    I16,
    I32,
    I64,
    ISize,
    U8,
    U16,
    U32,
    U64,
    USize,
    F32,
    F64,
    Char,
    Bool,
    Void,
    String,
    StringView,

    // Operators
    Assign,
    FatArrow,
    DeclareAssign,
    Add,
    Increment,
    Sub,
    Mul,
    Div,
    Mod,
    Xor,
    BitOr,
    BitNot,
    ShiftLeft,
    ShiftRight,
    Bang,
    Equal,
    NotEqual,
    StrictEqual,
    StrictNotEqual,
    Ampersand,
    RawAddress,  // Parser-composed &raw operator; raw remains contextual.
    And,
    Or,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    // Punctuation
    LParen,
    RParen,
    LBracket,
    RBracket,
    LCurly,
    RCurly,
    Comma,
    Dot,
    Arrow,
    LeftArrow,
    Scope,
    Colon,
    SemiColon,
    Hash,
    At,

    // Keywords
    Module,
    Test,
    Enum,
    TypeSet,
    Structure,
    Private,
    Override,
    Build,
    Func,
    Once,
    Unsafe,
    Export,
    Quiver,
    Extern,
    Inline,
    Comptime,
    Return,
    Defer,
    Yield,
    If,
    Match,
    Else,
    Also,
    While,
    For,
    Loop,
    Range,
    Proceed,
    Break,
    JumpTo,
    Error,
    Own,
    Lend,
    Const,
    ConstPtr,
    ConstRef,
    Volatile,
    As,

    // Built-in constants
    None,
    Null,
    True,
    False,

    // Identifiers
    Identifier,
    NumberLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,

    // End of file
    EOF_TOKEN,

    // No Token
    NoToken,

    // Invalid source retained for error recovery
    Invalid
};

#endif  // FLECHA_TOKENTYPE_HPP
