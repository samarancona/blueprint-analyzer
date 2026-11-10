#pragma once

#include "Templates/SharedPointer.h"

class FJsonObject;
class UNiagaraSystem;

namespace FNiagaraAnalyzer
{
    TSharedPtr<FJsonObject> Analyze(UNiagaraSystem* System);
}
