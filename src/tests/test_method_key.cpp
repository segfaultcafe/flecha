#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "semantic/method_key.hpp"
#include "semantic/symbol.hpp"

using namespace flecha::core;
using namespace flecha::semantic;

namespace {

SemanticType Receiver(const StructureType& owner, bool isConst = false,
                      SemanticTypeKind kind = SemanticTypeKind::Reference) {
    return SemanticType(kind, SemanticType(&owner, isConst));
}

SemanticType Receiver(const InterfaceType& owner, bool isConst = false,
                      SemanticTypeKind kind = SemanticTypeKind::Reference) {
    return SemanticType(kind, SemanticType(&owner, isConst));
}

CallableSignature SignatureFor(SemanticType receiver) {
    CallableSignature signature;
    signature.result = SemanticType(SemanticTypeKind::Reference,
                                    SemanticType(SemanticTypeKind::I32, true));
    signature.parameters = {receiver,
                            SemanticType(SemanticTypeKind::StringView),
                            SemanticType(SemanticTypeKind::Reference,
                                         SemanticType(SemanticTypeKind::I32))};
    signature.parameterContracts = {OwnershipContract::None,
                                    OwnershipContract::None,
                                    OwnershipContract::Lend};
    signature.resultContracts = {OwnershipContract::None};
    signature.borrowSourceIndices = {0, 2};
    signature.isUnsafe = true;
    return signature;
}

FunctionSymbol Method(const StructureType& owner, CallableSignature signature) {
    return FunctionSymbol(
        owner.name + "::inspect",
        std::make_shared<const CallableSignature>(std::move(signature)), {},
        BuiltInFunction::None, &owner, false, true);
}

}  // namespace

TEST(MethodKeyTests, ErasesNominalOwnerAndNormalizesBorrowSources) {
    InterfaceType interfaceOwner;
    interfaceOwner.name = "contracts::Readable";
    StructureType implementationOwner;
    implementationOwner.name = "models::Document";

    const FunctionSymbol implementationMethod = Method(
        implementationOwner, SignatureFor(Receiver(implementationOwner, true)));

    const MethodKey expected = NormalizeMethodKey(
        "inspect", SignatureFor(Receiver(interfaceOwner, true)), true);
    const MethodKey actual = MethodKeyOf(implementationMethod);

    EXPECT_EQ(expected, actual);
    EXPECT_EQ(actual.name, "inspect");
    EXPECT_EQ(actual.receiver, MethodReceiverKind::ConstReference);
    ASSERT_EQ(actual.signature.parameters.size(), 2);
    EXPECT_EQ(actual.signature.parameters[0].Kind(),
              SemanticTypeKind::StringView);
    EXPECT_TRUE(actual.borrowsReceiver);
    EXPECT_EQ(actual.signature.borrowSourceIndices,
              std::vector<std::size_t>({1}));
}

TEST(MethodKeyTests, DistinguishesReceiverParametersResultAndBorrowContract) {
    StructureType owner;
    owner.name = "Box";
    const MethodKey baseline =
        MethodKeyOf(Method(owner, SignatureFor(Receiver(owner, true))));

    CallableSignature mutableReceiver = SignatureFor(Receiver(owner, true));
    mutableReceiver.parameters[0] = Receiver(owner);
    EXPECT_NE(baseline, MethodKeyOf(Method(owner, mutableReceiver)));

    CallableSignature differentParameter = SignatureFor(Receiver(owner, true));
    differentParameter.parameters[1] = SemanticType(SemanticTypeKind::String);
    EXPECT_NE(baseline, MethodKeyOf(Method(owner, differentParameter)));

    CallableSignature differentResult = SignatureFor(Receiver(owner, true));
    differentResult.result = SemanticType(SemanticTypeKind::I32);
    differentResult.resultContracts = {OwnershipContract::None};
    EXPECT_NE(baseline, MethodKeyOf(Method(owner, differentResult)));

    CallableSignature differentBorrow = SignatureFor(Receiver(owner, true));
    differentBorrow.borrowSourceIndices = {2};
    EXPECT_NE(baseline, MethodKeyOf(Method(owner, differentBorrow)));

    CallableSignature differentOwnership = SignatureFor(Receiver(owner, true));
    differentOwnership.parameterContracts[2] = OwnershipContract::None;
    EXPECT_NE(baseline, MethodKeyOf(Method(owner, differentOwnership)));
}

TEST(MethodKeyTests, KeepsStaticVariadicAndEffectShapesInTheKey) {
    CallableSignature signature;
    signature.result = SemanticType(SemanticTypeKind::Void);
    signature.parameters = {SemanticType(SemanticTypeKind::I32)};

    const MethodKey ordinary =
        NormalizeMethodKey("send", signature, false, false, false);
    const MethodKey variadic =
        NormalizeMethodKey("send", signature, false, true, false);
    const MethodKey borrowedPack =
        NormalizeMethodKey("send", signature, false, true, true);

    EXPECT_EQ(ordinary.receiver, MethodReceiverKind::None);
    EXPECT_NE(ordinary, variadic);
    EXPECT_NE(variadic, borrowedPack);

    signature.isCrashable = true;
    EXPECT_NE(ordinary,
              NormalizeMethodKey("send", signature, false, false, false));
}
