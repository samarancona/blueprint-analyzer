#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// Object references are represented by their paths. Never recursively expand external assets.
TSharedRef<FJsonObject> DescribeNiagaraObject(const UObject* Object);
TSharedRef<FJsonObject> DescribeNiagaraSettings(const UStruct* Type, const void* Data);
TSharedPtr<FJsonValue> DescribeNiagaraStruct(const UScriptStruct* Type, const void* Data);
