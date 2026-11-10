#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UNiagaraScript;

// Definitions is shared by all stages in a report. Calls refer to a script path
// and its selected version so repeated modules only need one definition.
void AddNiagaraScriptAnalysis(TSharedRef<FJsonObject> Json, UNiagaraScript* Script,
    const FString& EmitterName, TSharedRef<FJsonObject> Definitions);
