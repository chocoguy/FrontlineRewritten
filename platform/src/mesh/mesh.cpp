// The Modern renderer's bin scopes (mesh.h).

#include "mesh.h"

#include "mesh_cache.h"
#include "native/render.h"
#include "os/os_internal.h"
#include "types.h"

namespace frontline::mesh {

int g_bin = NATIVE_BIN_NONE;

}  // namespace frontline::mesh

extern "C" void NativeMeshBin(int bin) {
    frontline::mesh::g_bin = bin > NATIVE_BIN_NONE && bin < NATIVE_BIN_COUNT ? bin : NATIVE_BIN_NONE;
}

extern "C" void NativeMeshForget(const void* list) {
    if (list != nullptr) {
        frontline::mesh::ForgetMesh(PTR_TO_ADDR(list));
    }
}

// Rebooting (os/reboot.cpp), after ResetGx waited for the GPU: the next run's
// lists are elsewhere, or the same addresses hold other lists.
void frontline::os::ResetMeshes() {
    frontline::mesh::g_bin = NATIVE_BIN_NONE;
    frontline::mesh::ForgetMeshes();
}
