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

test("namespace attachment has explicit keyword and namespace scopes", () => {
  const includes = grammar.patterns.map((pattern) => pattern.include);
  assert.ok(
    includes.indexOf("#namespaceAttachments") <
      includes.indexOf("#namespaces"),
  );
  const attachment = grammar.repository.namespaceAttachments;
  assert.match("under namespace root::foo;", new RegExp(attachment.begin));
  assert.equal(
    attachment.beginCaptures["1"].name,
    "keyword.control.module.flecha",
  );
  assert.equal(
    attachment.beginCaptures["3"].name,
    "keyword.control.module.flecha",
  );
  assert.equal(attachment.patterns[0].include, "#namespaces");
  assert.doesNotMatch("i32 under = 20;", new RegExp(attachment.begin));
  assert.doesNotMatch("i32 namespace = 22;", new RegExp(attachment.begin));
  assert.doesNotMatch(grammar.repository.control.match, /under|namespace/);
});

test("typed decoration clauses are contextual", () => {
  const declaration = grammar.repository.decorationDeclarations.patterns[0];
  assert.match("decoration meta {", new RegExp(declaration.begin));
  assert.match("decoration meta\n{", new RegExp(declaration.begin));
  assert.equal(declaration.beginCaptures["2"].name, "entity.name.type.decoration.flecha");
  const application = declaration.patterns[1];
  const target = declaration.patterns[2];
  assert.match("application(stringv name)", new RegExp(application.match));
  for (const kind of ["structure", "structure::field", "enum", "interface"]) {
    assert.match(`target ${kind}`, new RegExp(target.match));
  }
  assert.doesNotMatch(grammar.repository.control.match, /application|target/);
});

test("behavior headers distinguish the keyword and owning decoration", () => {
  const behavior = grammar.repository.behaviorDeclarations;
  for (const header of ["behavior json<T> => structure", "behavior json<T> => enum"]) {
    assert.match(header, new RegExp(behavior.match));
  }
  assert.equal(behavior.captures["1"].name, "keyword.control.flecha");
  assert.equal(behavior.captures["2"].name, "entity.name.type.behavior.flecha");
});

test("bounded field iteration scopes only contextual binders", () => {
  const iteration = grammar.repository.fieldIteration;
  assert.match("for<F> Field<T, F> field = T::fields() {", new RegExp(iteration.match));
  assert.match("for < Value > Field < Target, Value > descriptor", new RegExp(iteration.match));
  assert.doesNotMatch("i32 fields() { return 1; }", new RegExp(iteration.match));
  assert.doesNotMatch("Field value;", new RegExp(iteration.match));
  assert.equal(iteration.captures["2"].name, "entity.name.type.parameter.flecha");
  assert.equal(iteration.captures["6"].name, "variable.other.field-descriptor.flecha");
});

const fs = require("node:fs");
const path = require("node:path");
const asyncFixture = fs.readFileSync(
  path.join(__dirname, "../../fixtures/async.fl"), "utf8",
);

test("async declarations preserve modifier, result, and function scopes", () => {
  const declaration = grammar.repository.functionDeclarations;
  const match = asyncFixture.split("\n")
    .map((line) => new RegExp(declaration.begin).exec(line))
    .find((match) => match && match[5] === "calculate");
  assert.ok(match);
  assert.equal(match[2], "async ");
  assert.equal(match[3], "i32");
  assert.equal(declaration.beginCaptures["2"].name, "storage.modifier.flecha");
  assert.equal(declaration.beginCaptures["5"].name, "entity.name.function.flecha");
  assert.match("async i32 Worker::calculate()", new RegExp(grammar.repository.methodDeclarations.begin));
  assert.match("async", new RegExp(grammar.repository.modifiers.match));
  assert.doesNotMatch("asynchronous", new RegExp(grammar.repository.modifiers.match));
});

test("shoot and postfix await have contextual async scopes", () => {
  const patterns = grammar.repository.asyncOperations.patterns;
  const shoot = new RegExp(patterns[0].match);
  const awaitPattern = new RegExp(patterns[1].match);
  assert.equal(patterns[0].name, "keyword.control.async.flecha");
  assert.equal(patterns[1].captures["3"].name, "keyword.control.async.flecha");
  for (const source of ["shoot calculate()", "child.await", "other. await", "calculate().await"]) {
    assert.ok(asyncFixture.includes(source));
    assert.match(source, source.startsWith("shoot") ? shoot : awaitPattern);
  }
  for (const source of ["shooter", "spawn()", "Command::spawn()"]) {
    assert.doesNotMatch(source, shoot);
  }
  for (const source of ["i32 await = 0", "awaiting", "object.awaiting"]) {
    assert.doesNotMatch(source, awaitPattern);
  }
  const includes = grammar.patterns.map((pattern) => pattern.include);
  for (const earlier of ["#comments", "#strings", "#characters"]) {
    assert.ok(includes.indexOf(earlier) < includes.indexOf("#asyncOperations"));
  }
  assert.ok(includes.indexOf("#asyncOperations") < includes.indexOf("#functions"));
});

test("arrow creation and shift/minus retain operator scopes", () => {
  const operators = new RegExp(grammar.repository.operators.match, "g");
  for (const source of [">>- calculate()", "x >> -y", "x>>-y"]) {
    assert.ok(asyncFixture.includes(source));
    assert.equal(source.match(operators).join(""), ">>-");
  }
  assert.equal(grammar.repository.operators.name, "keyword.operator.flecha");
});

test("try is ordinary error propagation syntax", () => {
  const control = new RegExp(grammar.repository.control.match);
  assert.match("value := try parse();", control);
  assert.doesNotMatch("retry();", control);
});
