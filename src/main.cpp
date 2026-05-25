//==============================================================================
// Lulzfish - Main Entry Point
//==============================================================================
//
// This is the starting point for the Lulzfish chess engine.
// Currently a minimal skeleton that validates the build and core types.
//
// Future: Full UCI loop + search integration.
//==============================================================================

#include "lulzfish/core/types.hpp"
#include "lulzfish/core/bitboard.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/core/attacks.hpp"
#include "lulzfish/uci/uci.hpp"

#include <iostream>

int main() {
    std::cout << "Lulzfish v0.5 — Verified Correctness Baseline\n";
    std::cout << "Scalar attacks + search prototype + graph eval prototype.\n\n";

    lulzfish::core::init_attack_tables();

    lulzfish::uci::loop();

    return 0;
}
