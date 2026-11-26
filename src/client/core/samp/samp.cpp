#include "samp.hpp"

#include "common.hpp"
#include "hooks/hook_manager.hpp"
#include "samp/addresses.hpp"
#include "samp/components/game.hpp"
#include "system/logger.hpp"

static Samp* g_samp_self = nullptr;
static void* g_samp_init_return = nullptr;
static void* g_samp_deinit_return = nullptr;

static void __cdecl HandleSampInitialization() noexcept
{
    LOG_INFO("Loading SA-MP ...");

    g_samp_init_return = g_samp_self ? g_samp_self->GetOriginal("SampInitialization") : nullptr;
    if (g_samp_self)
        g_samp_self->DisableHook("SampInitialization");

    if (g_samp_self && g_samp_self->OnLoaded)
        g_samp_self->OnLoaded();

    LOG_INFO("SA-MP loaded.");
}

static void __cdecl HandleSampDeinitialization() noexcept
{
    LOG_INFO("Exit SA-MP ...");

    g_samp_deinit_return = g_samp_self ? g_samp_self->GetOriginal("SampDeinitialization") : nullptr;
    if (g_samp_self)
        g_samp_self->DisableHook("SampDeinitialization");

    if (g_samp_self && g_samp_self->OnExit)
        g_samp_self->OnExit();
}

static __declspec(naked) void SampInitializationHookThunk() noexcept
{
    __asm {
        pushad
        pushfd
        call HandleSampInitialization
        popfd
        popad
        jmp g_samp_init_return
    }
}

static __declspec(naked) void SampDeinitializationHookThunk() noexcept
{
    __asm {
        pushad
        pushfd
        call HandleSampDeinitialization
        popfd
        popad
        jmp g_samp_deinit_return
    }
}

bool Samp::Initialize()
{
    g_samp_self = this;

    void* addrInit = SampAddresses::Instance().SampInitialization();
    void* addrDeinit = SampAddresses::Instance().SampDeinitialization();

    if (!addrInit || !addrDeinit)
    {
        LOG_FATAL("One or more SA-MP addresses are null.");
        return false;
    }

    if (!hooks_.Install("SampInitialization", addrInit, reinterpret_cast<void*>(&SampInitializationHookThunk)) ||
        !hooks_.Install("SampDeinitialization", addrDeinit, reinterpret_cast<void*>(&SampDeinitializationHookThunk)))
    {
        LOG_FATAL("Failed to hook Samp.");
        return false;
    }

    InitializeComponents();

    return true;
}

void* Samp::GetOriginal(const char* name) const
{
    return hooks_.GetOriginal(name);
}

void Samp::DisableHook(const char* name)
{
    hooks_.Disable(name);
}