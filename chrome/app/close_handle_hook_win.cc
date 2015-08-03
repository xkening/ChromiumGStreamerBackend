// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/app/close_handle_hook_win.h"

#include <Windows.h>
#include <psapi.h>

#include <vector>

#include "base/lazy_instance.h"
#include "base/memory/scoped_ptr.h"
#include "base/win/iat_patch_function.h"
#include "base/win/pe_image.h"
#include "base/win/scoped_handle.h"
#include "chrome/common/channel_info.h"
#include "components/version_info/version_info.h"

namespace {

typedef BOOL (WINAPI* CloseHandleType) (HANDLE handle);
CloseHandleType g_close_function = NULL;

// The entry point for CloseHandle interception. This function notifies the
// verifier about the handle that is being closed, and calls the original
// function.
BOOL WINAPI CloseHandleHook(HANDLE handle) {
  base::win::OnHandleBeingClosed(handle);
  return g_close_function(handle);
}

// Provides a simple way to temporarily change the protection of a memory page.
class AutoProtectMemory {
 public:
  AutoProtectMemory()
      : changed_(false), address_(NULL), bytes_(0), old_protect_(0) {}

  ~AutoProtectMemory() {
    RevertProtection();
  }

  // Grants write access to a given memory range.
  bool ChangeProtection(void* address, size_t bytes);

  // Restores the original page protection.
  void RevertProtection();

 private:
  bool changed_;
  void* address_;
  size_t bytes_;
  DWORD old_protect_;

  DISALLOW_COPY_AND_ASSIGN(AutoProtectMemory);
};

bool AutoProtectMemory::ChangeProtection(void* address, size_t bytes) {
  DCHECK(!changed_);
  DCHECK(address);

  // Change the page protection so that we can write.
  MEMORY_BASIC_INFORMATION memory_info;
  if (!VirtualQuery(address, &memory_info, sizeof(memory_info)))
    return false;

  DWORD is_executable = (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY) &
                        memory_info.Protect;

  DWORD protect = is_executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  if (!VirtualProtect(address, bytes, protect, &old_protect_))
    return false;

  changed_ = true;
  address_ = address;
  bytes_ = bytes;
  return true;
}

void AutoProtectMemory::RevertProtection() {
  if (!changed_)
    return;

  DCHECK(address_);
  DCHECK(bytes_);

  VirtualProtect(address_, bytes_, old_protect_, &old_protect_);
  changed_ = false;
  address_ = NULL;
  bytes_ = 0;
  old_protect_ = 0;
}

// Performs an EAT interception.
void EATPatch(HMODULE module, const char* function_name,
              void* new_function, void** old_function) {
  if (!module)
    return;

  base::win::PEImage pe(module);
  if (!pe.VerifyMagic())
    return;

  DWORD* eat_entry = pe.GetExportEntry(function_name);
  if (!eat_entry)
    return;

  if (!(*old_function))
    *old_function = pe.RVAToAddr(*eat_entry);

  AutoProtectMemory memory;
  if (!memory.ChangeProtection(eat_entry, sizeof(DWORD)))
    return;

  // Perform the patch.
#pragma warning(push)
#pragma warning(disable: 4311)
  // These casts generate warnings because they are 32 bit specific.
  *eat_entry = reinterpret_cast<DWORD>(new_function) -
               reinterpret_cast<DWORD>(module);
#pragma warning(pop)
}

// Keeps track of all the hooks needed to intercept CloseHandle.
class CloseHandleHooks {
 public:
  CloseHandleHooks() {}
  ~CloseHandleHooks() {}

  void AddIATPatch(HMODULE module);
  void AddEATPatch();
  void Unpatch();

 private:
  std::vector<base::win::IATPatchFunction*> hooks_;
  DISALLOW_COPY_AND_ASSIGN(CloseHandleHooks);
};
base::LazyInstance<CloseHandleHooks> g_hooks = LAZY_INSTANCE_INITIALIZER;

void CloseHandleHooks::AddIATPatch(HMODULE module) {
  if (!module)
    return;

  base::win::IATPatchFunction* patch = new base::win::IATPatchFunction;
  __try {
    // There is no guarantee that |module| is still loaded at this point.
    if (patch->PatchFromModule(module, "kernel32.dll", "CloseHandle",
                               CloseHandleHook)) {
      delete patch;
      return;
    }
  } __except((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
              GetExceptionCode() == EXCEPTION_GUARD_PAGE ||
              GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR) ?
             EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    // Leak the patch.
    return;
  }

  hooks_.push_back(patch);
  if (!g_close_function) {
    // Things are probably messed up if each intercepted function points to
    // a different place, but we need only one function to call.
    g_close_function =
      reinterpret_cast<CloseHandleType>(patch->original_function());
  }
}

void CloseHandleHooks::AddEATPatch() {
  // An attempt to restore the entry on the table at destruction is not safe.
  EATPatch(GetModuleHandleA("kernel32.dll"), "CloseHandle",
           &CloseHandleHook, reinterpret_cast<void**>(&g_close_function));
}

void CloseHandleHooks::Unpatch() {
  for (std::vector<base::win::IATPatchFunction*>::iterator it = hooks_.begin();
       it != hooks_.end(); ++it) {
    (*it)->Unpatch();
    delete *it;
  }
}

bool UseHooks() {
#if defined(ARCH_CPU_X86_64)
  return false;
#elif defined(NDEBUG)
  version_info::Channel channel = chrome::GetChannel();
  if (channel == version_info::Channel::CANARY ||
      channel == version_info::Channel::DEV ||
      channel == version_info::Channel::UNKNOWN) {
    return true;
  }

  return false;
#else  // NDEBUG
  return true;
#endif
}

void PatchLoadedModules(CloseHandleHooks* hooks) {
  const DWORD kSize = 256;
  DWORD returned;
  scoped_ptr<HMODULE[]> modules(new HMODULE[kSize]);
  if (!EnumProcessModules(GetCurrentProcess(), modules.get(),
                          kSize * sizeof(HMODULE), &returned)) {
    return;
  }
  returned /= sizeof(HMODULE);
  returned = std::min(kSize, returned);

  for (DWORD current = 0; current < returned; current++) {
    hooks->AddIATPatch(modules[current]);
  }
}

}  // namespace

void InstallCloseHandleHooks() {
  if (UseHooks()) {
    CloseHandleHooks* hooks = g_hooks.Pointer();

    // Performing EAT interception first is safer in the presence of other
    // threads attempting to call CloseHandle.
    hooks->AddEATPatch();
    PatchLoadedModules(hooks);
  }
}

void RemoveCloseHandleHooks() {
  // We are partching all loaded modules without forcing them to stay in memory,
  // removing patches is not safe.
}
