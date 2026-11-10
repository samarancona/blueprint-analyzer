#include "NiagaraAnalyzerProperties.h"

#include "Dom/JsonObject.h"
#include "NiagaraCommon.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

namespace
{
using FValue = TSharedPtr<FJsonValue>;
using FValues = TArray<FValue>;

FValue ReadStruct(const UScriptStruct* Type, const void* Data, int32 Depth);
FValue ReadProperty(const FProperty* Property, const void* Data, int32 Depth);

TSharedRef<FJsonObject> ReadFields(const UStruct* Type, const void* Data, bool bEditableOnly, int32 Depth)
{
    auto Result = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Type, EFieldIterationFlags::IncludeSuper); It; ++It)
    {
        const FProperty* Field = *It;
        if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_SkipSerialization) ||
            (bEditableOnly && !Field->HasAnyPropertyFlags(CPF_Edit)))
        {
            continue;
        }
        if (Field->ArrayDim == 1)
        {
            Result->SetField(Field->GetName(), ReadProperty(Field, Field->ContainerPtrToValuePtr<void>(Data), Depth));
        }
        else
        {
            FValues Entries;
            for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
            {
                Entries.Add(ReadProperty(Field, Field->ContainerPtrToValuePtr<void>(Data, Index), Depth));
            }
            Result->SetArrayField(Field->GetName(), Entries);
        }
    }
    return Result;
}

FValue ReadStruct(const UScriptStruct* Type, const void* Data, int32 Depth)
{
    if (Depth >= 32)
    {
        return MakeShared<FJsonValueString>(TEXT("[nested value depth limit]"));
    }
    if (Type->IsChildOf(FNiagaraVariableBase::StaticStruct()))
    {
        const auto& Variable = *static_cast<const FNiagaraVariableBase*>(Data);
        if (Variable.GetName().IsNone()) return MakeShared<FJsonValueNull>();
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Name"), Variable.GetName().ToString());
        if (Variable.GetType().IsValid())
        {
            Json->SetStringField(TEXT("Type"), Variable.GetType().GetName());
        }
        return MakeShared<FJsonValueObject>(Json);
    }
    if (Type == FNiagaraVariableAttributeBinding::StaticStruct())
    {
        const auto& Binding = *static_cast<const FNiagaraVariableAttributeBinding*>(Data);
        FName Name = Binding.GetParamMapBindableVariable().GetName();
        if (Name.IsNone()) Name = Binding.GetName();
        if (Name.IsNone()) return MakeShared<FJsonValueNull>();
        return MakeShared<FJsonValueString>(Name.ToString());
    }
    if (Type == TBaseStructure<FGuid>::Get())
    {
        return MakeShared<FJsonValueString>(static_cast<const FGuid*>(Data)->ToString());
    }
    if (Type == FNiagaraBool::StaticStruct())
    {
        return MakeShared<FJsonValueBoolean>(static_cast<const FNiagaraBool*>(Data)->GetValue());
    }
    if (Type == FInstancedStruct::StaticStruct())
    {
        const auto& Instance = *static_cast<const FInstancedStruct*>(Data);
        if (!Instance.IsValid()) return MakeShared<FJsonValueNull>();
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Type"), Instance.GetScriptStruct()->GetPathName());
        Json->SetField(TEXT("Value"), ReadStruct(Instance.GetScriptStruct(), Instance.GetMemory(), Depth + 1));
        return MakeShared<FJsonValueObject>(Json);
    }
    return MakeShared<FJsonValueObject>(ReadFields(Type, Data, false, Depth + 1));
}

FValue ReadProperty(const FProperty* Property, const void* Data, int32 Depth)
{
    if (const auto* Struct = CastField<FStructProperty>(Property))
    {
        return ReadStruct(Struct->Struct, Data, Depth);
    }
    if (const auto* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(Array, Data);
        FValues Items;
        Items.Reserve(Helper.Num());
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
        {
            Items.Add(ReadProperty(Array->Inner, Helper.GetRawPtr(Index), Depth + 1));
        }
        return MakeShared<FJsonValueArray>(Items);
    }
    if (const auto* Bool = CastField<FBoolProperty>(Property))
    {
        return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(Data));
    }
    if (const auto* Number = CastField<FNumericProperty>(Property); Number && !Number->IsEnum())
    {
        if (Number->IsFloatingPoint())
        {
            const double Value = Number->GetFloatingPointPropertyValue(Data);
            if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
        }
        else if (Number->GetSize() <= sizeof(int32))
        {
            return MakeShared<FJsonValueNumber>(static_cast<double>(Number->GetSignedIntPropertyValue(Data)));
        }
    }
    if (const auto* Object = CastField<FObjectPropertyBase>(Property); Object && !CastField<FSoftObjectProperty>(Property))
    {
        const UObject* Ref = Object->GetObjectPropertyValue(Data);
        if (!IsValid(Ref)) return MakeShared<FJsonValueNull>();
        return MakeShared<FJsonValueString>(Ref->GetPathName());
    }
    FString Text;
    Property->ExportTextItem_Direct(Text, Data, nullptr, nullptr, PPF_None);
    return MakeShared<FJsonValueString>(Text);
}
}

TSharedPtr<FJsonValue> DescribeNiagaraStruct(const UScriptStruct* Type, const void* Data)
{
    return ReadStruct(Type, Data, 0);
}

TSharedRef<FJsonObject> DescribeNiagaraSettings(const UStruct* Type, const void* Data)
{
    return ReadFields(Type, Data, true, 0);
}

TSharedRef<FJsonObject> DescribeNiagaraObject(const UObject* Object)
{
    auto Result = MakeShared<FJsonObject>();
    if (IsValid(Object))
    {
        Result->SetStringField(TEXT("Name"), Object->GetName());
        Result->SetStringField(TEXT("Class"), Object->GetClass()->GetPathName());
        Result->SetStringField(TEXT("Path"), Object->GetPathName());
        Result->SetObjectField(TEXT("Settings"), DescribeNiagaraSettings(Object->GetClass(), Object));
    }
    return Result;
}
