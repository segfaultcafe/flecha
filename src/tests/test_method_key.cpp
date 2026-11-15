#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "semantic/method_contract.hpp"
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

TEST(MethodKeyTests, ErasesNominalOwnerAndKeepsOverloadShape) {
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
    ASSERT_EQ(actual.parameters.size(), 2);
    EXPECT_EQ(actual.parameters[0].Kind(), SemanticTypeKind::StringView);
}

TEST(MethodKeyTests, DistinguishesOnlyReceiverParametersAndVariadicShape) {
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
    EXPECT_EQ(baseline, MethodKeyOf(Method(owner, differentResult)));

    CallableSignature differentBorrow = SignatureFor(Receiver(owner, true));
    differentBorrow.borrowSourceIndices = {2};
    EXPECT_EQ(baseline, MethodKeyOf(Method(owner, differentBorrow)));

    CallableSignature differentOwnership = SignatureFor(Receiver(owner, true));
    differentOwnership.parameterContracts[2] = OwnershipContract::None;
    EXPECT_EQ(baseline, MethodKeyOf(Method(owner, differentOwnership)));

    CallableSignature differentEffect = SignatureFor(Receiver(owner, true));
    differentEffect.isCrashable = true;
    EXPECT_EQ(baseline, MethodKeyOf(Method(owner, differentEffect)));
}

TEST(MethodKeyTests, KeepsStaticAndVariadicShapesInTheKey) {
    CallableSignature signature;
    signature.result = SemanticType(SemanticTypeKind::Void);
    signature.parameters = {SemanticType(SemanticTypeKind::I32)};

    const MethodKey ordinary = NormalizeMethodKey("send", signature, false);
    const MethodKey variadic =
        NormalizeMethodKey("send", signature, false, true);

    EXPECT_EQ(ordinary.receiver, MethodReceiverKind::None);
    EXPECT_NE(ordinary, variadic);

    signature.isCrashable = true;
    EXPECT_EQ(ordinary, NormalizeMethodKey("send", signature, false));
}

TEST(MethodContractTests, ComparesCompleteContractsWithoutCategory) {
    StructureType owner;
    owner.name = "Box";
    const CallableSignature signature = SignatureFor(Receiver(owner, true));
    const MethodContract baseline = NormalizeMethodContract(
        "inspect", signature, true, false, false, MethodVisibility::Protected,
        MethodCategory::Virtual);

    CallableSignature reordered = signature;
    reordered.borrowSourceIndices = {2, 0, 2};
    const MethodContract equivalent = NormalizeMethodContract(
        "inspect", reordered, true, false, false, MethodVisibility::Protected,
        MethodCategory::Final);
    EXPECT_TRUE(baseline.SameContract(equivalent));

    CallableSignature differentResult = signature;
    differentResult.result = SemanticType(SemanticTypeKind::I32);
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentResult, true, false, false,
                                MethodVisibility::Protected)));

    CallableSignature differentBorrow = signature;
    differentBorrow.borrowSourceIndices = {2};
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentBorrow, true, false, false,
                                MethodVisibility::Protected)));

    CallableSignature differentParameterOwnership = signature;
    differentParameterOwnership.parameterContracts[2] = OwnershipContract::None;
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentParameterOwnership, true,
                                false, false, MethodVisibility::Protected)));

    CallableSignature differentResultOwnership = signature;
    differentResultOwnership.resultContracts[0] = OwnershipContract::Own;
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentResultOwnership, true,
                                false, false, MethodVisibility::Protected)));

    EXPECT_FALSE(baseline.SameContract(NormalizeMethodContract(
        "inspect", signature, true, false, false, MethodVisibility::Public)));

    EXPECT_FALSE(baseline.SameContract(NormalizeMethodContract(
        "inspect", signature, true, false, true, MethodVisibility::Protected)));

    CallableSignature differentEffect = signature;
    differentEffect.isUnsafe = false;
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentEffect, true, false, false,
                                MethodVisibility::Protected)));

    differentEffect = signature;
    differentEffect.isOnce = true;
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentEffect, true, false, false,
                                MethodVisibility::Protected)));

    differentEffect = signature;
    differentEffect.isCrashable = true;
    EXPECT_FALSE(baseline.SameContract(
        NormalizeMethodContract("inspect", differentEffect, true, false, false,
                                MethodVisibility::Protected)));
}
