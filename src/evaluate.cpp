/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "incbin/incbin.h"
#include "misc.h"
#include "nnue/evaluate_nnue.h"
#include "nnue/nnue_architecture.h"
#include "position.h"
#include "thread.h"
#include "types.h"
#include "uci.h"

#include <random>
#include <chrono>

// Macro to embed the default efficiently updatable neural network (NNUE) file
// data in the engine binary (using incbin.h, by Dale Weiler).
// This macro invocation will declare the following three variables
//     const unsigned char        gEmbeddedNNUEData[];  // a pointer to the embedded data
//     const unsigned char *const gEmbeddedNNUEEnd;     // a marker to the end
//     const unsigned int         gEmbeddedNNUESize;    // the size of the embedded file
// Note that this does not work in Microsoft Visual Studio.
#if !defined(_MSC_VER) && !defined(NNUE_EMBEDDING_OFF)
INCBIN(EmbeddedNNUEBig, EvalFileDefaultNameBig);
INCBIN(EmbeddedNNUESmall, EvalFileDefaultNameSmall);
#else
const unsigned char        gEmbeddedNNUEBigData[1]   = {0x0};
const unsigned char* const gEmbeddedNNUEBigEnd       = &gEmbeddedNNUEBigData[1];
const unsigned int         gEmbeddedNNUEBigSize      = 1;
const unsigned char        gEmbeddedNNUESmallData[1] = {0x0};
const unsigned char* const gEmbeddedNNUESmallEnd     = &gEmbeddedNNUESmallData[1];
const unsigned int         gEmbeddedNNUESmallSize    = 1;
#endif


namespace Stockfish {

namespace Eval {

std::string       currentEvalFileName[2] = {"None", "None"};
const std::string EvFiles[2]             = {"EvalFile", "EvalFileSmall"};
const std::string EvFileNames[2]         = {EvalFileDefaultNameBig, EvalFileDefaultNameSmall};

int NNUE::RandomEvalPerturb = 0;
int NNUE::waitms = 0;
  
// Tries to load a NNUE network at startup time, or when the engine
// receives a UCI command "setoption name EvalFile value nn-[a-z0-9]{12}.nnue"
// The name of the NNUE network is always retrieved from the EvalFile option.
// We search the given network in three locations: internally (the default
// network may be embedded in the binary), in the active working directory and
// in the engine directory. Distro packagers may define the DEFAULT_NNUE_DIRECTORY
// variable to have the engine search in a special directory in their distro.
void NNUE::init() {

    for (NetSize netSize : {Big, Small})
    {
        std::string eval_file = std::string(Options[EvFiles[netSize]]);
      
        if (eval_file.empty())
            eval_file = EvFileNames[netSize];

#if defined(DEFAULT_NNUE_DIRECTORY)
        std::vector<std::string> dirs = {"<internal>", "", CommandLine::binaryDirectory,
                                         stringify(DEFAULT_NNUE_DIRECTORY)};
#else
        std::vector<std::string> dirs = {"<internal>", "", CommandLine::binaryDirectory};
#endif

        for (const std::string& directory : dirs)
        {
            if (currentEvalFileName[netSize] != eval_file)
            {
                if (directory != "<internal>")
                {
                    std::ifstream stream(directory + eval_file, std::ios::binary);
                    if (NNUE::load_eval(eval_file, stream, netSize))
                        currentEvalFileName[netSize] = eval_file;
                }

                if (directory == "<internal>" && eval_file == EvFileNames[netSize])
                {
                    // C++ way to prepare a buffer for a memory stream
                    class MemoryBuffer: public std::basic_streambuf<char> {
                       public:
                        MemoryBuffer(char* p, size_t n) {
                            setg(p, p, p + n);
                            setp(p, p + n);
                        }
                    };

                    MemoryBuffer buffer(
                      const_cast<char*>(reinterpret_cast<const char*>(
                        netSize == Small ? gEmbeddedNNUESmallData : gEmbeddedNNUEBigData)),
                      size_t(netSize == Small ? gEmbeddedNNUESmallSize : gEmbeddedNNUEBigSize));
                    (void) gEmbeddedNNUEBigEnd;  // Silence warning on unused variable
                    (void) gEmbeddedNNUESmallEnd;

                    std::istream stream(&buffer);
                    if (NNUE::load_eval(eval_file, stream, netSize))
                        currentEvalFileName[netSize] = eval_file;
                }
            }
        }
    }
}

// Verifies that the last net used was loaded successfully
void NNUE::verify() {

    for (NetSize netSize : {Big, Small})
    {
        std::string eval_file = std::string(Options[EvFiles[netSize]]);
      
        if (eval_file.empty())
            eval_file = EvFileNames[netSize];

        if (currentEvalFileName[netSize] != eval_file)
        {
            std::string msg1 =
              "Network evaluation parameters compatible with the engine must be available.";
            std::string msg2 = "The network file " + eval_file + " was not loaded successfully.";
            std::string msg3 = "The UCI option EvalFile might need to specify the full path, "
                               "including the directory name, to the network file.";
            std::string msg4 = "The default net can be downloaded from: "
                               "https://tests.stockfishchess.org/api/nn/"
                             + std::string(EvFileNames[netSize]);
            std::string msg5 = "The engine will be terminated now.";

            sync_cout << "info string ERROR: " << msg1 << sync_endl;
            sync_cout << "info string ERROR: " << msg2 << sync_endl;
            sync_cout << "info string ERROR: " << msg3 << sync_endl;
            sync_cout << "info string ERROR: " << msg4 << sync_endl;
            sync_cout << "info string ERROR: " << msg5 << sync_endl;

            exit(EXIT_FAILURE);
        }

        sync_cout << "info string NNUE evaluation using " << eval_file << sync_endl;
    }
}
}

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the given color. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos, Color c) {
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}


// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Position& pos) {

    assert(!pos.checkers());

    int   v;
    Color stm        = pos.side_to_move();
    int   shuffling  = pos.rule50_count();
    int   simpleEval = simple_eval(pos, stm);

    bool lazy = abs(simpleEval) > 2550;

    if (lazy)
        v = simpleEval;
    else
    {
        bool smallNet = abs(simpleEval) > 1050;
        int nnueComplexity;

        Value nnue = smallNet ? NNUE::evaluate<NNUE::Small>(pos, true, &nnueComplexity)
                              : NNUE::evaluate<NNUE::Big>(pos, true, &nnueComplexity);

        int optimism = pos.this_thread()->optimism[stm];

        // Blend optimism and eval with nnue complexity and material imbalance
        optimism += optimism * (nnueComplexity + std::abs(simpleEval - nnue)) / 512;
        nnue -= nnue * (nnueComplexity + std::abs(simpleEval - nnue)) / 32768;

        int npm = pos.non_pawn_material() / 64;
        v       = (nnue * (915 + npm + 9 * pos.count<PAWN>()) + optimism * (154 + npm)) / 1024;
    }

    // Damp down the evaluation linearly when shuffling
    v = v * (200 - shuffling) / 214;

    // SFnps Begin //
    if((NNUE::RandomEvalPerturb) || (NNUE::waitms))
    {
      // waitms millisecs
      std::this_thread::sleep_for(std::chrono::milliseconds(NNUE::waitms));

      // RandomEval
      static thread_local std::mt19937_64 rng = [](){return std::mt19937_64(std::time(0));}();
      std::normal_distribution<float> d(0.0, PawnValue);
      float r = d(rng);
      r = std::clamp<float>(r, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
      v = (NNUE::RandomEvalPerturb * Value(r) + (100 - NNUE::RandomEvalPerturb) * v) / 100;
    }
    // SFnps End //

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(int(v), VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    // Reset any global variable used in eval
    pos.this_thread()->bestValue       = VALUE_ZERO;
    pos.this_thread()->rootSimpleEval  = VALUE_ZERO;
    pos.this_thread()->optimism[WHITE] = VALUE_ZERO;
    pos.this_thread()->optimism[BLACK] = VALUE_ZERO;

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
    ss << '\n' << NNUE::trace(pos) << '\n';

    ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

    Value v;
    v = NNUE::evaluate<NNUE::Big>(pos, false);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "NNUE evaluation        " << 0.01 * UCI::to_cp(v) << " (white side)\n";

    v = evaluate(pos);
    v = pos.side_to_move() == WHITE ? v : -v;
    ss << "Final evaluation       " << 0.01 * UCI::to_cp(v) << " (white side)";
    ss << " [with scaled NNUE, ...]";
    ss << "\n";

    return ss.str();
}

}  // namespace Stockfish
