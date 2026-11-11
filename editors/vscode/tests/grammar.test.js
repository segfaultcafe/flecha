const assert = require("node:assert/strict");
const test = require("node:test");

const grammar = require("../syntaxes/flecha.tmLanguage.json");

test("nominal composition has stable scopes across line breaks", () => {
  const includes = grammar.patterns.map((pattern) => pattern.include);
  assert.ok(
    includes.indexOf("#nominalDeclarations") <
      includes.indexOf("#methodDeclarations"),
  );

  const declaration = grammar.repository.nominalDeclarations;
  const declarationStart = new RegExp(declaration.begin);
  const inheritance = declaration.patterns[0];
  const inheritanceMatch = new RegExp(inheritance.match);

  assert.match("structure Inline << Parent {};", declarationStart);
  assert.match("structure Wrapped", declarationStart);
  assert.match("    << Parent {", inheritanceMatch);
  assert.equal(
    inheritance.captures["1"].name,
    "keyword.operator.inheritance.flecha",
  );
  assert.equal(
    inheritance.captures["3"].name,
    "entity.other.inherited-class.flecha",
  );

  // The declaration context must survive the first newline. This is what
  // keeps the inline and wrapped forms on the same TextMate code path.
  assert.doesNotMatch(declaration.end, /\$/);
  assert.match(grammar.repository.operators.match, /<</);
});
