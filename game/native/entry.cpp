// What the platform layer calls in the game image (frontline::LoadGame finds
// these by name): its main(), and its operator delete for game memory freed
// from host threads (NativeFreeLater). The image keeps operator new and
// delete to itself, so the platform layer can't call them directly.

int FrontlineGameMain();

extern "C" __attribute__((visibility("default"))) int FrontlineGameEntry() { return FrontlineGameMain(); }

extern "C" __attribute__((visibility("default"))) void FrontlineGameFree(void* p) { ::operator delete(p); }
