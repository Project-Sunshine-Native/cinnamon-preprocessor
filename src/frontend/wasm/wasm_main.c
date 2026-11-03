#include "frontend/preprocess_frontend.h"
#include "preprocess_wasm.h"

int main(int argc, char** argv) {
    initWasmProgress();
    return runFrontend(argc, argv);
}
