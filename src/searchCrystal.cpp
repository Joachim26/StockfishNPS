/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"
#include "nnue/evaluate_nnue.h"

namespace Stockfish {

namespace Search {

  LimitsType Limits;
}

namespace Tablebases {

  int Cardinality;
  bool RootInTB;
  bool UseRule50;
}

namespace TB = Tablebases;

using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  // Different node types, used as a template parameter
  enum NodeType { NonPV, PV, Root };

  // Futility margin
  Value futility_margin(Depth d, bool improving) {
    return Value(140 * (d - improving));
  }

  // Reductions lookup table, initialized at startup
  int Reductions[MAX_MOVES]; // [depth or moveNumber]

  Depth reduction(bool i, Depth d, int mn, Value delta, Value rootDelta) {
    int r = Reductions[d] * Reductions[mn];
    return (r + 1372 - int(delta) * 1073 / int(rootDelta)) / 1024 + (!i && r > 936);
  }

  constexpr int futility_move_count(bool improving, Depth depth) {
    return improving ? (3 + depth * depth)
                     : (3 + depth * depth) / 2;
  }

  // History and stats update bonus, based on depth
  int stat_bonus(Depth d) {
    return std::min(336 * d - 547, 1561);
  }

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);

  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = 0);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply, int r50c);
  void update_pv(Move* pv, Move move, const Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus);
  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

} // namespace


/// Search::init() is called at startup to initialize various lookup tables

void Search::init() {

  for (int i = 1; i < MAX_MOVES; ++i)
      Reductions[i] = int((20.57 + std::log(Threads.size()) / 2) * std::log(i));
}


/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();

  Time.availableNodes = 0;
  TT.clear();
  Threads.clear();
  Tablebases::init(Options["SyzygyPath"]); // Free mapped files
}


/// MainThread::search() is started when the program receives the UCI 'go'
/// command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {

  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }

  Color us = rootPos.side_to_move();
  Time.init(Limits, us, rootPos.game_ply());
  TT.new_search();

  Eval::NNUE::verify();

  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE, -VALUE_MATE : VALUE_DRAW, VALUE_DRAW)
                << sync_endl;
  }
  else
  {
      Threads.start_searching(); // start non-main threads
      Thread::search();          // main thread start searching
  }

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands.

  while (!Threads.stop && (ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  Threads.wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.availableNodes += Limits.inc[us] - Threads.nodes_searched();

  Thread* bestThread = this;

  if (    int(Options["MultiPV"]) == 1
      && !Limits.depth
      &&  rootMoves[0].pv[0] != MOVE_NONE)
      bestThread = Threads.get_best_thread();

  bestPreviousScore = bestThread->rootMoves[0].score;
  bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

  // Send again PV info if we have a new best thread
  if (bestThread != this)
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth) << sync_endl;

  sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

  std::cout << sync_endl;
}


/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  // To allow access to (ss-7) up to (ss+2), the stack must be oversized.
  // The former is needed to allow update_continuation_histories(ss-1, ...),
  // which accesses its argument at ss-6, also near the root.
  // The latter is needed for statScore and killer initialization.
  Stack stack[MAX_PLY+10], *ss = stack+7;
  Move  pv[MAX_PLY+1];
  Value alpha, beta, delta;
  Move  lastBestMove = MOVE_NONE;
  Depth lastBestMoveDepth = 0;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1, totBestMoveChanges = 0;
  int iterIdx = 0;

  std::memset(ss-7, 0, 10 * sizeof(Stack));
  for (int i = 7; i > 0; --i)
  {
      (ss-i)->continuationHistory = &this->continuationHistory[0][0][NO_PIECE][0]; // Use as a sentinel
      (ss-i)->staticEval = VALUE_NONE;
  }

  for (int i = 0; i <= MAX_PLY + 2; ++i)
      (ss+i)->ply = i;

  ss->pv = pv;

  bestValue = -VALUE_INFINITE;

  if (mainThread)
  {
      if (mainThread->bestPreviousScore == VALUE_INFINITE)
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = VALUE_ZERO;
      else
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = mainThread->bestPreviousScore;
  }

  size_t multiPV = size_t(Options["MultiPV"]);

  multiPV = std::min(multiPV, rootMoves.size());

  // Iterative deepening loop until requested to stop or the target depth is reached
  while (   ++rootDepth < MAX_PLY
         && !Threads.stop
         && !(Limits.depth && mainThread && rootDepth > Limits.depth))
  {
      // Age out PV variability metric
      if (mainThread)
          totBestMoveChanges /= 2;

      // Save the last iteration's scores before first PV line is searched and
      // all the move scores except the (new) PV are set to -VALUE_INFINITE.
      for (RootMove& rm : rootMoves)
          rm.previousScore = rm.score;

      size_t pvFirst = 0;
      pvLast = 0;

      // MultiPV loop. We perform a full root search for each PV line
      for (pvIdx = 0; pvIdx < multiPV && !Threads.stop; ++pvIdx)
      {
          if (pvIdx == pvLast)
          {
              pvFirst = pvLast;
              for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                  if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                      break;
          }

          // Reset UCI info selDepth for each depth and each PV line
          selDepth = 0;

          // Reset aspiration window starting size
          Value prev = rootMoves[pvIdx].averageScore;
          int momentum = int(prev) * prev / 15799;
          delta = Value(10);

          if (prev > VALUE_MATE_IN_MAX_PLY)
              alpha = VALUE_MATE_IN_MAX_PLY - MAX_PLY;
          else
              alpha = std::max(prev - (delta + (prev < 0 ? momentum : 0)),-VALUE_INFINITE);

          beta  = std::min(prev + (delta + (prev > 0 ? momentum : 0)), VALUE_INFINITE);

          // Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we don't fail
          // high/low anymore.
          while (true)
          {
              bestValue = Stockfish::search<Root>(rootPos, ss, alpha, beta, rootDepth, false);

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

              // If search has been stopped, we break immediately. Sorting is
              // safe because RootMoves is still valid, although it refers to
              // the previous iteration.
              if (Threads.stop)
                  break;

              // When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha)
              {
                  beta = (alpha + beta) / 2;
                  alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                  if (mainThread)
                      mainThread->stopOnPonderhit = false;
              }
              else if (bestValue >= beta)
                  beta = std::min(bestValue + delta, VALUE_INFINITE);

              else
                  break;

              delta += delta / 3;

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

          if (    mainThread
              && (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth) << sync_endl;
      }

      if (!Threads.stop)
          completedDepth = rootDepth;

      if (rootMoves[0].pv[0] != lastBestMove)
      {
          lastBestMove = rootMoves[0].pv[0];
          lastBestMoveDepth = rootDepth;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && bestValue >= VALUE_MATE_IN_MAX_PLY
          && VALUE_MATE - bestValue <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      // Use part of the gained time from a previous stable move for the current move
      for (Thread* th : Threads)
      {
          totBestMoveChanges += th->bestMoveChanges;
          th->bestMoveChanges = 0;
      }

      // Do we have time for the next iteration? Can we stop searching now?
      if (    Limits.use_time_management()
          && !Threads.stop
          && !mainThread->stopOnPonderhit)
      {
          double fallingEval = (69 + 13 * (mainThread->bestPreviousAverageScore - bestValue)
                                   +  6 * (mainThread->iterValue[iterIdx] - bestValue)) / 619.6;

          fallingEval = std::clamp(fallingEval, 0.5, 1.5);

          // If the bestMove is stable over several iterations, reduce time accordingly
          timeReduction = lastBestMoveDepth + 6 < completedDepth ? 0.68
                                                                 : (mainThread->previousTimeReduction == 0.68 ? 2.20
                                                                                                              : 1.52);

          double bestMoveInstability = 1 + totBestMoveChanges / 8;

          TimePoint elapsedT = Time.elapsed();
          TimePoint optimumT = Time.optimum();

          // Stop the search if we have only one legal move, or if available time elapsed
          if (   (rootMoves.size() == 1 && (elapsedT > optimumT / 16))
              || elapsedT > 4.33 * optimumT
              || elapsedT > optimumT * fallingEval * timeReduction * bestMoveInstability)
          {
              // If we are allowed to ponder do not stop the search now but
              // keep pondering until the GUI sends "ponderhit" or "stop".
              if (mainThread->ponder)
                  mainThread->stopOnPonderhit = true;
              else
                  Threads.stop = true;
          }
      }

      mainThread->iterValue[iterIdx] = bestValue;
      iterIdx = (iterIdx + 1) & 3;
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;
}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove, bestMove;
    Depth extension, newDepth, ttDepth;
    Bound ttBound;
    Value bestValue, value, ttValue, eval, probCutBeta;
    bool givesCheck, improving, priorCapture, isMate, gameCycle;
    bool capture, moveCountPruning,
         ttCapture, kingDanger, ourMove, nullParity, singularQuietLMR;
    Piece movedPiece;
    int moveCount, captureCount, quietCount, improvement, rootDepth;

    // Step 1. Initialize node
    Thread* thisThread  = pos.this_thread();
    ss->inCheck         = pos.checkers();
    priorCapture        = pos.captured_piece();
    Color us            = pos.side_to_move();
    moveCount           = captureCount = quietCount = ss->moveCount = 0;
    bestValue           = -VALUE_INFINITE;
    gameCycle           = kingDanger = false;
    rootDepth           = thisThread->rootDepth;
    ourMove             = !(ss->ply & 1);
    nullParity          = (ourMove == thisThread->nmpSide);
    ss->secondaryLine   = false;

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    thisThread->nodes++;

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    // Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttDepth = tte->depth();
    ttBound = tte->bound();
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ss->ttHit    ? tte->move() : MOVE_NONE;
    ttCapture = ttMove && pos.capture(ttMove);

    // At this point, if excluded, skip straight to step 6, static eval. However,
    // to save indentation, we list the condition in all code between here and there.
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());

    if (!rootNode)
    {
        // Check if we have an upcoming move which draws by repetition, or
        // if the opponent had an alternative move earlier to this position.
        if (pos.has_game_cycle(ss->ply))
        {
            if (!excludedMove)
                tte->save(posKey, VALUE_DRAW, ss->ttPv, BOUND_EXACT,
                          depth, MOVE_NONE, VALUE_NONE);

            if (VALUE_DRAW >= beta)
                return VALUE_DRAW;

            gameCycle = true;
            alpha = std::max(alpha, VALUE_DRAW);
        }

        // Step 2. Check for aborted search and immediate draw
        if (pos.is_draw(ss->ply))
            return VALUE_DRAW;

        if (Threads.stop.load(std::memory_order_relaxed) || ss->ply >= MAX_PLY)
            return ss->ply >= MAX_PLY && !ss->inCheck ? evaluate(pos)
                                                      : VALUE_DRAW;

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        if (alpha >= mate_in(ss->ply+1))
            return mate_in(ss->ply+1);

    }
    else
        thisThread->rootDelta = beta - alpha;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->excludedMove = bestMove = MOVE_NONE;
    (ss+2)->killers[0]   = (ss+2)->killers[1] = MOVE_NONE;
    (ss+2)->cutoffCnt    = 0;
    ss->doubleExtensions = (ss-1)->doubleExtensions;
    Square prevSq        = is_ok((ss-1)->currentMove) ? to_sq((ss-1)->currentMove) : SQ_NONE;
    ss->statScore        = 0;

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && !excludedMove
        && !gameCycle
        && (!ourMove || beta < VALUE_MATE_IN_MAX_PLY)
        && ttDepth > depth - (ttBound == BOUND_EXACT)
        && ttValue != VALUE_NONE // Possible in case of TT access race or if !ttHit
        && (ttValue != VALUE_DRAW || VALUE_DRAW >= beta)
        && (ttBound & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit (~2 Elo)
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                // Bonus for a quiet ttMove that fails high (~2 Elo)
                if (!ttCapture)
                    update_quiet_stats(pos, ss, ttMove, stat_bonus(depth));

                // Extra penalty for early quiet moves of the previous ply (~0 Elo on STC, ~2 Elo on LTC)
                if (prevSq != SQ_NONE && (ss-1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            }
            // Penalty for a quiet ttMove that fails low (~1 Elo)
            else if (!ttCapture)
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][from_to(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }

        return ttValue;
    }

    // Step 5. Tablebases probe
    if (!rootNode && !excludedMove && TB::Cardinality)
    {
        int piecesCount = popcount(pos.pieces());

        if (    piecesCount <= TB::Cardinality
            &&  pos.rule50_count() == 0
            && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore v = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (thisThread == Threads.main())
                static_cast<MainThread*>(thisThread)->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                int drawScore = TB::UseRule50 ? 1 : 0;

                int centiPly = TraditionalPawnValue * ss->ply / 100;

                Value tbValue =    v < -drawScore ? -VALUE_TB_WIN + (10 * TraditionalPawnValue * (v == -1)) + centiPly + TraditionalPawnValue * popcount(pos.pieces( pos.side_to_move()))
                                 : v >  drawScore ?  VALUE_TB_WIN - (10 * TraditionalPawnValue * (v ==  1)) - centiPly - TraditionalPawnValue * popcount(pos.pieces(~pos.side_to_move()))
                                 : v < 0 ? Value(-56) : VALUE_DRAW;

                if (    abs(v) <= drawScore
                    || !ss->ttHit
                    || (v < -drawScore && beta  > tbValue + 19)
                    || (v >  drawScore && alpha < tbValue - 19))
                {
                    tte->save(posKey, tbValue, ss->ttPv, v > drawScore ? BOUND_LOWER : v < -drawScore ? BOUND_UPPER : BOUND_EXACT,
                              v == 0 ? MAX_PLY : depth, MOVE_NONE, VALUE_NONE);

                    return tbValue;
                }
            }
        }
    }

    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    kingDanger = ourMove ? false : pos.king_danger();

    // Step 6. Static evaluation of the position
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        improving = false;
        improvement = 0;
    }
    else
    {
    if (excludedMove)
    {
        // Providing the hint that this node's accumulator will be used often brings significant Elo gain (13 Elo)
        Eval::NNUE::hint_common_parent_position(pos);
        eval = ss->staticEval;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        ss->staticEval = eval = tte->eval();
        if (eval == VALUE_NONE)
            ss->staticEval = eval = evaluate(pos);
        else if (PvNode)
            Eval::NNUE::hint_common_parent_position(pos);

        // ttValue can be used as a better position evaluation (~7 Elo)
        if (    ttValue != VALUE_NONE
            && (ttMove != MOVE_NONE || ttValue <= eval)
            && (ttBound & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        ss->staticEval = eval = evaluate(pos);
        // Save static evaluation into transposition table
        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, eval);
    }

    // Use static evaluation difference to improve quiet move ordering (~4 Elo)
    if (is_ok((ss-1)->currentMove) && !(ss-1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-18 * int((ss-1)->staticEval + ss->staticEval), -1817, 1817);
        thisThread->mainHistory[~us][from_to((ss-1)->currentMove)] << bonus;
    }

    // Set up the improvement variable, which is the difference between the current
    // static evaluation and the previous static evaluation at our turn (if we were
    // in check at our previous move we look at the move prior to it). The improvement
    // margin and the improving flag are used in various pruning heuristics.
    improvement =   (ss-2)->staticEval != VALUE_NONE ? ss->staticEval - (ss-2)->staticEval
                  : (ss-4)->staticEval != VALUE_NONE ? ss->staticEval - (ss-4)->staticEval
                  :                                    173;
    improving = improvement > 0;

    // Begin early pruning.
    if (   !PvNode
        && (ourMove || !excludedMove)
        && !thisThread->nmpGuardV
        &&  abs(eval) < 2 * VALUE_KNOWN_WIN)
    {
       // Step 7. Razoring.
       // If eval is really low check with qsearch if it can exceed alpha, if it can't,
       // return a fail low.
       if (  !ourMove
           && eval < alpha - 456 - 252 * depth * depth)
       {
        value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
        if (value < alpha)
            return value;
       }
       // Step 8. Futility pruning: child node (~25 Elo)
       if (    depth < 9 // was 8
           && !ss->ttPv
           && !kingDanger
           && !gameCycle
           && !(thisThread->nmpGuard && nullParity)
           &&  abs(alpha) < VALUE_KNOWN_WIN
           &&  eval >= beta
           &&  eval - futility_margin(depth, improving) - (ss-1)->statScore / 306 >= beta)
           return eval;

       // Step 9. Null move search with verification search (~22 Elo)
       if (   !thisThread->nmpGuard
           &&  (ss-1)->statScore < 17329
           && !gameCycle
           //&&  beta < VALUE_MATE_IN_MAX_PLY Implied by eval >= beta & abs(eval) < 2 * VALUE_KNOWN_WIN
           &&  eval >= beta
           &&  eval >= ss->staticEval
           &&  ss->staticEval >= beta - 21 * depth - improvement / 13 + 258
           &&  pos.non_pawn_material(us)
           && !kingDanger
           && (rootDepth < 11 || ourMove || MoveList<LEGAL>(pos).size() > 5))
       {
           assert(eval - beta >= 0);

           thisThread->nmpSide = ourMove;

           // Null move dynamic reduction based on depth and value
           Depth R = std::min(int(eval - beta) / 173, 6) + depth / 3 + 4;

           if (!ourMove && (ss-1)->secondaryLine)
               R = std::min(R, 8);

           if (   depth < 11
               || ttValue >= beta
               || ttDepth < depth-R
               || !(ttBound & BOUND_UPPER))
           {
           ss->currentMove = MOVE_NULL;
           ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

           pos.do_null_move(st);
           thisThread->nmpGuard = true;
           Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);
           thisThread->nmpGuard = false;
           pos.undo_null_move();

           if (nullValue >= beta)
           {
               // Do not return unproven mate or TB scores
               nullValue = std::min(nullValue, VALUE_MATE_IN_MAX_PLY);

               if (   depth < 11
                   && beta <= qsearch<NonPV>(pos, ss, beta-1, beta))
                   return nullValue;

               // Do verification search at high depths
               thisThread->nmpGuardV = true;
               Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false);
               thisThread->nmpGuardV = false;

               // While it is unsafe to return mate scores from null search, mate scores
               // from verification search are fine.
               if (v >= beta)
                   return v > VALUE_MATE_IN_MAX_PLY ? v : nullValue;
           }
           }
       }

       probCutBeta = beta + 168 - 61 * improving;

       // Step 10. ProbCut (~10 Elo)
       // If we have a good enough capture and a reduced search returns a value
       // much above beta, we can (almost) safely prune the previous move.
       if (    depth > 4
           &&  abs(beta) < VALUE_MATE_IN_MAX_PLY
           && (ttCapture || !ttMove)
           // If we don't have a ttHit or our ttDepth is not greater our
           // reduced depth search, continue with the probcut.
           && (!ss->ttHit || ttDepth < depth - 3))
       {
           assert(probCutBeta < VALUE_INFINITE);
           MovePicker mp(pos, ttMove, KnightValueMg - BishopValueMg + PieceValue[MG][type_of(pos.captured_piece())], &captureHistory);

           while ((move = mp.next_move()) != MOVE_NONE)
               if (move != excludedMove)
               {
                   assert(pos.capture(move) || promotion_type(move) == QUEEN);
                   assert(depth >= 5);

                   ss->currentMove = move;
                   ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                             [true]
                                                                             [pos.moved_piece(move)]
                                                                             [to_sq(move)];

                   pos.do_move(move, st);

                   // Perform a preliminary qsearch to verify that the move holds
                   value = -qsearch<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1);

                   // If the qsearch held perform the regular search
                   if (value >= probCutBeta)
                       value = -search<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1, depth - 4, !cutNode);

                   pos.undo_move(move);

                   if (value >= probCutBeta)
                   {
                       if (!excludedMove)
                           tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv,
                                     BOUND_LOWER, depth - 3, move, ss->staticEval);

                       return value;
                   }
               }

           Eval::NNUE::hint_common_parent_position(pos);
       }
    } // End early Pruning

    // Step 11. If the position is not in TT, decrease depth by 2 or 1 depending on node type (~3 Elo)
    if (   PvNode
        && depth >= 3
        && !gameCycle
        && !ttMove
        && (ss-1)->moveCount > 1)
        depth -= 2;

    if (    cutNode
        && !(ss-1)->secondaryLine
        &&  depth >= 7
        && !ttMove)
        depth -= 2;

    } // In check search starts here

   // Step 12. A small Probcut idea, when we are in check (~4 Elo)
   probCutBeta = beta + 413;
   if (    ss->inCheck
        && !PvNode
        && ttCapture
        && !gameCycle
        && !kingDanger
        && !(ss-1)->secondaryLine
        && !(thisThread->nmpGuard && nullParity)
        && !(thisThread->nmpGuardV && nullParity)
        && (ttBound & BOUND_LOWER)
        && ttDepth >= depth - 4
        && ttValue >= probCutBeta
        && abs(ttValue) <= VALUE_KNOWN_WIN
        && abs(beta) <= VALUE_KNOWN_WIN)
        return probCutBeta;

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    Move countermove = prevSq != SQ_NONE ? thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] : MOVE_NONE;

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers);

    value = bestValue;
    moveCountPruning = singularQuietLMR = false;

    // Indicate PvNodes that will probably fail low if the node was searched
    // at a depth equal or greater than the current depth, and the result of this search was a fail low.
    bool likelyFailLow =    PvNode
                         && ttMove
                         && (ttBound & BOUND_UPPER)
                         && ttDepth >= depth;

    bool lmPrunable = (  !ourMove
                       || ss->ply > 6
                       || (ss-1)->moveCount > 1
                       || (ss-3)->moveCount > 1
                       || (ss-5)->moveCount > 1);

    int lmrAdjustment =   ttCapture
                        + 2 * cutNode
                        + ((ss+1)->cutoffCnt > 3)
                        - (2 + (cutNode && ttDepth >= depth + 3)) * (ss->ttPv && !likelyFailLow)
                        - ((ss-1)->moveCount > 8)
                        - 2 * PvNode;

    bool allowLMR =     depth > 1
                    && !gameCycle
                    && (!PvNode || ss->ply > 1);

    bool doSingular =    !rootNode
                      && !excludedMove // Avoid recursive singular search
                      &&  ttValue != VALUE_NONE
                      && (ttBound & BOUND_LOWER)
                      &&  alpha > VALUE_MATED_IN_MAX_PLY + MAX_PLY
                      &&  ttValue > -VALUE_KNOWN_WIN / 2
                      &&  ttDepth >= depth - 3
                      &&  depth >= 4 - (thisThread->completedDepth > 22) + 2 * (PvNode && tte->is_pv());

    bool doLMP =    !PvNode
                 && (lmPrunable || ss->ply > 2)
                 &&  pos.non_pawn_material(us);

    // Step 13. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;

      ss->moveCount = ++moveCount;

      if (rootNode && thisThread == Threads.main() && Time.elapsed() > 3000)
          sync_cout << "info depth " << depth
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = 0;
      capture = pos.capture_stage(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);
      isMate = false;


      // This tracks all of our possible responses to our opponent's best moves outside of the PV.
      // The reasoning here is that while we look for flaws in the PV, we must otherwise find an improvement
      // in a secondary root move in order to change the PV. Such an improvement must occur on the path of
      // our opponent's best moves or else it is meaningless.
      ss->secondaryLine = (   (rootNode && moveCount > 1)
                           || (!ourMove && (ss-1)->secondaryLine && !excludedMove && moveCount == 1)
                           || ( ourMove && (ss-1)->secondaryLine));

      if (givesCheck)
      {
          pos.do_move(move, st, givesCheck);
          isMate = MoveList<LEGAL>(pos).size() == 0;
          pos.undo_move(move);
      }

      if (isMate)
      {
          ss->currentMove = move;
          ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                    [capture]
                                                                    [movedPiece]
                                                                    [to_sq(move)];
          value = mate_in(ss->ply+1);

          if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
          {
              (ss+1)->pv = pv;
              (ss+1)->pv[0] = MOVE_NONE;
          }
      }
      else
      {
      // If we already have a mate in 1 from the current position and the current
      // move isn't a mate in 1, continue as there is no point to searching it.
      if (bestValue >= mate_in(ss->ply+1))
          continue;

      // Calculate new depth for this move
      newDepth = depth - 1;
      Value delta = beta - alpha;
      Depth r = reduction(improving, depth, moveCount, delta, thisThread->rootDelta);

      // Step 14. Pruning at shallow depth (~120 Elo). Depth conditions are important for mate finding.
      if (   doLMP
          && (bestValue < VALUE_MATE_IN_MAX_PLY || !ourMove)
          && bestValue > VALUE_MATED_IN_MAX_PLY)
      {
          // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~8 Elo)
          moveCountPruning = moveCount >= futility_move_count(improving, depth);

          if (lmPrunable)
          {
          // Reduced depth of the next LMR search
          int lmrDepth = newDepth - r;

          if (   capture
              || givesCheck)
          {
              // Futility pruning for captures (~2 Elo)
              if (   !givesCheck
                  //&& !PvNode
                  &&  lmrDepth < 7 // was 3
                  && !ss->inCheck
                  && ss->staticEval + 197 + 248 * lmrDepth + PieceValue[EG][pos.piece_on(to_sq(move))]
                   + captureHistory[movedPiece][to_sq(move)][type_of(pos.piece_on(to_sq(move)))] / 7 < alpha)
                  continue;

              Bitboard occupied;
              // SEE based pruning (~11 Elo)
              if (/*(!ourMove || !(ss-1)->secondaryLine) &&*/ !pos.see_ge(move, occupied, Value(-205) * depth))
              {
                 if (depth < 2 - capture)
                    continue;
                 // Don't prune the move if opponent Queen/Rook is under discovered attack after the exchanges
                 // Don't prune the move if opponent King is under discovered attack after or during the exchanges
                 Bitboard leftEnemies = (pos.pieces(~us, KING, QUEEN, ROOK)) & occupied;
                 Bitboard attacks = 0;
                 occupied |= to_sq(move);
                 while (leftEnemies && !attacks)
                 {
                      Square sq = pop_lsb(leftEnemies);
                      attacks |= pos.attackers_to(sq, occupied) & pos.pieces(us) & occupied;
                      // don't consider pieces which were already threatened/hanging before SEE exchanges
                      if (attacks && (sq != pos.square<KING>(~us) && (pos.attackers_to(sq, pos.pieces()) & pos.pieces(us))))
                         attacks = 0;
                 }
                 if (!attacks)
                    continue;
              }
          }
          else
          {
              int history =   (*contHist[0])[movedPiece][to_sq(move)]
                            + (*contHist[1])[movedPiece][to_sq(move)]
                            + (*contHist[3])[movedPiece][to_sq(move)];

              // Continuation history based pruning (~2 Elo)
              if (   lmrDepth < 6
                  && history < -3832 * depth)
                  continue;

              history += 2 * thisThread->mainHistory[us][from_to(move)];

              lmrDepth += history / 7011;
              lmrDepth = std::max(lmrDepth, -2);

              // Futility pruning: parent node (~13 Elo)
              if (   !ss->inCheck
                  && lmrDepth < 12 // was 8
                  && history < 20500 - 3875 * (depth - 1)
                  //&& (!ourMove || !(ss-1)->secondaryLine)
                  && ss->staticEval + 112 + 138 * lmrDepth <= alpha)
                  continue;

              lmrDepth = std::max(lmrDepth, 0);

              // Prune moves with negative SEE (~4 Elo)
              if (!pos.see_ge(move, Value(-27 * lmrDepth * lmrDepth - 16 * lmrDepth)))
                  continue;
          }
          }
      }

      // Step 15. Extensions (~100 Elo)
      // Singular extension search (~94 Elo). If all moves but one fail low on a
      // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
      // then that move is singular and should be extended. To verify this we do
      // a reduced search on all the other moves but the ttMove and if the
      // result is lower than ttValue minus a margin, then we will extend the ttMove.
      if (    doSingular
          &&  move == ttMove)
      {
          Value singularBeta = std::max(ttValue - (82 + 65 * (ss->ttPv && !PvNode)) * depth / 64, VALUE_MATED_IN_MAX_PLY);
          Depth singularDepth = (depth - 1) / 2;

          ss->excludedMove = move;
          // the search with excludedMove will update ss->staticEval
          value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
          ss->excludedMove = MOVE_NONE;

          if (value < singularBeta)
          {
              singularQuietLMR = !ttCapture;
              // Avoid search explosion by limiting the number of double extensions
              if (  !PvNode
                  && value < singularBeta - 21
                  && ss->doubleExtensions < 4)
              {
                  extension = 2;
                  depth += depth < 13;
              }
              else
                  extension = 1;
          }

          // Multi-cut pruning
          // Our ttMove is assumed to fail high, and now we failed high also on a reduced
          // search without the ttMove. So we assume this expected Cut-node is not singular,
          // that multiple moves fail high, and we can prune the whole subtree by returning
          // a soft bound.
          else if (!PvNode)
          {
            if (ttValue >= beta)
                return ttValue; // beta safer?

            // If the eval of ttMove is less than alpha and value, we reduce it (negative extension)
            else if (!gameCycle && alpha < VALUE_MATE_IN_MAX_PLY - MAX_PLY)
                     extension = -1;
          }
      }

      // Check extensions (~1 Elo)
      if (   extension < 1
          && givesCheck
          && depth > 9)
          extension = 1;

      // Quiet ttMove extensions (~1 Elo)
      else if (   extension < 1
               && PvNode
               && move == ttMove
               && move == ss->killers[0]
               && (*contHist[0])[movedPiece][to_sq(move)] >= 5168)
          extension = 1;

      // Add extension to new depth
      newDepth += extension;
      ss->doubleExtensions = (ss-1)->doubleExtensions + (extension == 2);

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [capture]
                                                                [movedPiece]
                                                                [to_sq(move)];

      // Step 16. Make the move
      pos.do_move(move, st, givesCheck);

      bool lateKingDanger = (rootDepth > 10 && ourMove && ss->ply < 7 && pos.king_danger());

      ss->statScore =  2 * thisThread->mainHistory[us][from_to(move)]
                         + (*contHist[0])[movedPiece][to_sq(move)]
                         + (*contHist[1])[movedPiece][to_sq(move)]
                         + (*contHist[3])[movedPiece][to_sq(move)]
                         - 4006;

      r =         r
                + lmrAdjustment
                - singularQuietLMR
                - (move == ttMove)
                - ss->statScore / (11124 + 4740 * (depth > 5 && depth < 22));

      // Step 17. Late moves reduction / extension (LMR, ~117 Elo)
      // We use various heuristics for the sons of a node after the first son has
      // been searched. In general we would like to reduce them, but there are many
      // cases where we extend a son if it has good chances to be "interesting".
      if (    allowLMR
          && !lateKingDanger
          &&  moveCount > 1
          && (!capture || (cutNode && (ss-1)->moveCount > 1)))
      {
          // In general we want to cap the LMR depth search at newDepth, but when
          // reduction is negative, we allow this move a limited search extension
          // beyond the first move depth. This may lead to hidden double extensions.
          Depth d = std::clamp(newDepth - r, 1, newDepth + 1);

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true);

          // Do full depth search when reduced LMR search fails high
          if (value > alpha && d < newDepth)
          {
              // Adjust full depth search based on LMR results - if result
              // was good enough search deeper, if it was bad enough search shallower
              const bool doDeeperSearch = value > (bestValue + 64 + 11 * (newDepth - d));
              const bool doEvenDeeperSearch = value > alpha + 711 && ss->doubleExtensions <= 6;
              const bool doShallowerSearch = value < bestValue + newDepth;

              ss->doubleExtensions = ss->doubleExtensions + doEvenDeeperSearch;

              newDepth += doDeeperSearch - doShallowerSearch + doEvenDeeperSearch;

              if (newDepth > d)
                  value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

              int bonus = value <= alpha ? -stat_bonus(newDepth)
                        : value >= beta  ?  stat_bonus(newDepth)
                                         :  0;

              update_continuation_histories(ss, movedPiece, to_sq(move), bonus);
          }
      }

      // Step 18. Full depth search when LMR is skipped. If expected reduction is high, reduce its depth by 1.
      else if (!PvNode || moveCount > 1)
      {
          // Increase reduction for cut nodes and not ttMove (~1 Elo)
          if (!ttMove && cutNode)
              r += 2;

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth - (r > 3), !cutNode);
      }

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          if (gameCycle && (ss-1)->moveCount < 2)
              newDepth += 2;

          value = -search<PV>(pos, ss+1, -beta, -alpha, newDepth, false);
      }

      // Step 19. Undo move
      pos.undo_move(move);
      }

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 20. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
          return VALUE_ZERO;

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          if (abs(value) < VALUE_TB_WIN - 7 * TraditionalPawnValue)
              rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;
          else
              rm.averageScore = value;

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score =  rm.uciScore = value;
              rm.selDepth = thisThread->selDepth;
              rm.scoreLowerbound = rm.scoreUpperbound = false;

              thisThread->pvValue = value;

              if (value >= beta)
              {
                  rm.scoreLowerbound = true;
                  rm.uciScore = beta;
              }
              else if (value <= alpha)
              {
                  rm.scoreUpperbound = true;
                  rm.uciScore = alpha;
              }

              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each iteration.
              // This information is used for time management. In MultiPV mode,
              // we must take care to only do this for the first PV line.
              if (   moveCount > 1
                  && !thisThread->pvIdx)
                  ++thisThread->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (value >= beta)
              {
                  ss->cutoffCnt += 1 + !ttMove;
                  assert(value >= beta); // Fail high
                  break;
              }
              else
              {
                  // Reduce other moves if we have found at least one score improvement (~1 Elo)
                  // Reduce more for depth > 3 and depth < 12 (~1 Elo)
                  if (   depth > 1
                      && !gameCycle
                      && beta  <  VALUE_KNOWN_WIN
                      && alpha > -VALUE_KNOWN_WIN)
                      depth -= 1;

                  assert(depth > 0);
                  alpha = value; // Update alpha! Always alpha < beta
              }
          }
      }


      // If the move is worse than some previously searched move, remember it to update its stats later
      if (move != bestMove)
      {
          if (capture && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!capture && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 21. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha :
                    ss->inCheck  ? mated_in(ss->ply)
                                 : VALUE_DRAW;

    // If there is a move which produces search value greater than alpha we update stats of searched moves
    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         quietsSearched, quietCount, capturesSearched, captureCount, depth);

    // Bonus for prior countermove that caused the fail low
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonus = (depth > 5) + (PvNode || cutNode) + (bestValue < alpha - 113 * depth) + ((ss-1)->moveCount > 12);
        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth) * bonus);
    }

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree. (~7 Elo)
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss-1)->ttPv && depth > 3);

    // Write gathered information in transposition table
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main search
  // function with zero depth, or recursively with further decreasing depth per call.
  // (~155 Elo)
  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    Move pv[MAX_PLY+1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Bound ttBound;
    Value bestValue, value, ttValue, futilityValue, futilityBase;
    bool pvHit, givesCheck, capture, gameCycle;
    int moveCount;

    // Step 1. Initialize node
    if (PvNode)
    {
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    bestMove = MOVE_NONE;
    ss->inCheck = pos.checkers();
    moveCount = 0;
    gameCycle = false;

    thisThread->nodes++;

    if (pos.has_game_cycle(ss->ply))
    {
       if (VALUE_DRAW >= beta)
           return VALUE_DRAW;

       alpha = std::max(alpha, VALUE_DRAW);
       gameCycle = true;
    }

    if (pos.is_draw(ss->ply))
        return VALUE_DRAW;

    // Step 2. Check for an immediate draw or maximum ply reached
    if (ss->ply >= MAX_PLY)
        return !ss->inCheck ? evaluate(pos) : VALUE_DRAW;

    if (alpha >= mate_in(ss->ply+1))
        return mate_in(ss->ply+1);

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                      : DEPTH_QS_NO_CHECKS;

    // Step 3. Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttBound = tte->bound();
    ttMove = ss->ttHit ? tte->move() : MOVE_NONE;
    pvHit = ss->ttHit && tte->is_pv();

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ss->ttHit
        && !gameCycle
        && ((ss->ply & 1) || beta < VALUE_MATE_IN_MAX_PLY)
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race or if !ttHit
        && (ttValue != VALUE_DRAW || VALUE_DRAW >= beta)
        && (ttBound & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    // Step 4. Static evaluation of the position
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                ss->staticEval = bestValue = evaluate(pos);

            // ttValue can be used as a better position evaluation (~13 Elo)
            if (    ttValue != VALUE_NONE
                && (ttMove != MOVE_NONE || ttValue <= bestValue)
                && (ttBound & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
            // In case of null move search use previous static eval with a different sign
            ss->staticEval = bestValue = (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                                                          : -(ss-1)->staticEval;

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            // Save gathered info in transposition table
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 200;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    Square prevSq = is_ok((ss-1)->currentMove) ? to_sq((ss-1)->currentMove) : SQ_NONE;
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      prevSq);

    int quietCheckEvasions = 0;

    // Step 5. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != MOVE_NONE)
    {
        assert(is_ok(move));

        // Check for legality
        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture = pos.capture_stage(move);

        moveCount++;

      if (bestValue > VALUE_MATED_IN_MAX_PLY)
      {
         // Futility pruning and moveCount pruning (~10 Elo)
         if (   !givesCheck
             &&  to_sq(move) != prevSq
             &&  futilityBase > -VALUE_KNOWN_WIN
             &&  type_of(move) != PROMOTION)
         {
             if (moveCount > 2 + PvNode)
                 continue;

             futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

             if (futilityValue <= alpha)
             {
                 bestValue = std::max(bestValue, futilityValue);
                 continue;
             }

             if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
             {
                 bestValue = std::max(bestValue, futilityBase);
                 continue;
             }
         }

         // We prune after 2nd quiet check evasion where being 'in check' is implicitly checked through the counter
         // and being a 'quiet' apart from being a tt move is assumed after an increment because captures are pushed ahead.
         if (quietCheckEvasions > 1)
             break;

         // Continuation history based pruning (~3 Elo)
         if (   !capture
             && !PvNode
             && (*contHist[0])[pos.moved_piece(move)][to_sq(move)] < 0
             && (*contHist[1])[pos.moved_piece(move)][to_sq(move)] < 0)
            continue;

         // Do not search moves with bad enough SEE values (~5 Elo)
         if (!pos.see_ge(move, Value(-95)))
             continue;
      }

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

        // Update the current move
        ss->currentMove = move;
        ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                  [capture]
                                                                  [pos.moved_piece(move)]
                                                                  [to_sq(move)];

        quietCheckEvasions += !capture && ss->inCheck;

        // Step 7. Make and search the move
        pos.do_move(move, st, givesCheck);
        value = -qsearch<nodeType>(pos, ss+1, -beta, -alpha, depth - 1);
        pos.undo_move(move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 8. Check for a new best move
        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                if (PvNode) // Update pv even in fail-high case
                    update_pv(ss->pv, move, (ss+1)->pv);

                if (PvNode && value < beta) // Update alpha here!
                    alpha = value;
                else
                    break; // Fail high
            }
        }
    }

    // Step 9. Check for mate
    // All legal moves have been searched. A special case: if we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());

        return mated_in(ss->ply); // Plies to mate from the root
    }

    // Save gathered info in transposition table
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
  // "plies to mate from the current position". Standard scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v > VALUE_MATE_IN_MAX_PLY  ? v + ply
          : v < VALUE_MATED_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
  // from the transposition table (which refers to the plies to mate/be mated from
  // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
  // for mate scores, to avoid potentially false mate scores related to the 50 moves rule
  // and the graph history interaction, we return an optimal TB score instead.

  Value value_from_tt(Value v, int ply, int r50c) {

    /*return  v == VALUE_NONE             ? VALUE_NONE
          : v >= VALUE_MATE_IN_MAX_PLY  ? v - ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v + ply : v; */

    if (v == VALUE_NONE)
        return VALUE_NONE;

    if (v > VALUE_MATE_IN_MAX_PLY)  // TB win or better
    {
        if (VALUE_MATE - v > 99 - r50c)
            return VALUE_MATE_IN_MAX_PLY; // do not return a potentially false mate score

        return v - ply;
    }

    if (v < VALUE_MATED_IN_MAX_PLY) // TB loss or worse
    {
        if (VALUE_MATE + v > 99 - r50c)
            return VALUE_MATED_IN_MAX_PLY; // do not return a potentially false mate score

        return v + ply;
    }

    return v;
  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, const Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_all_stats() updates stats at the end of search() when a bestMove is found

  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    Piece moved_piece = pos.moved_piece(bestMove);
    PieceType captured;

    int bonus1 = stat_bonus(depth + 1);

    if (!pos.capture_stage(bestMove))
    {
        int bonus2 = bestValue > beta + 145 ? bonus1               // larger bonus
                                            : stat_bonus(depth);   // smaller bonus

        // Increase stats for the best move in case it was a quiet move
        update_quiet_stats(pos, ss, bestMove, bonus2);

        // Decrease stats for all non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            thisThread->mainHistory[us][from_to(quietsSearched[i])] << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), to_sq(quietsSearched[i]), -bonus2);
        }
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        captured = type_of(pos.piece_on(to_sq(bestMove)));
        captureHistory[moved_piece][to_sq(bestMove)][captured] << bonus1;
    }

    // Extra penalty for a quiet early move that was not a TT move or
    // main killer move in previous ply when it gets refuted.
    if (   prevSq != SQ_NONE
        && ((ss-1)->moveCount == 1 + (ss-1)->ttHit || ((ss-1)->currentMove == (ss-1)->killers[0]))
        && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -bonus1);

    // Decrease stats for all non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured = type_of(pos.piece_on(to_sq(capturesSearched[i])));
        captureHistory[moved_piece][to_sq(capturesSearched[i])][captured] << -bonus1;
    }
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, -4, and -6 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4, 6})
    {
        // Only update first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (is_ok((ss-i)->currentMove))
            (*(ss-i)->continuationHistory)[pc][to] << bonus;
    }
  }


  // update_quiet_stats() updates move sorting heuristics

  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus) {

    // Update killers
    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][from_to(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    // Update countermove history
    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }
  }

} // namespace


/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint lastInfoTime = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tick = Limits.startTime + elapsed;

  if (tick - lastInfoTime >= 1000)
  {
      lastInfoTime = tick;
      dbg_print();
  }

  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();
  uint64_t tbHits = Threads.tb_hits() + (TB::RootInTB ? rootMoves.size() : 0);

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = rootMoves[i].score != -VALUE_INFINITE;

      if (depth == 1 && !updated && i > 0)
          continue;

      Depth d = updated ? depth : std::max(1, depth - 1);
      Value v = updated ? rootMoves[i].uciScore : rootMoves[i].previousScore;
      Value v2 = rootMoves[i].previousScore;

      if (v == -VALUE_INFINITE)
          v = VALUE_ZERO;

      bool tb = TB::RootInTB && abs(v) < VALUE_TB_WIN - 6 * TraditionalPawnValue;

      v = tb ? rootMoves[i].tbScore : v;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v, v2);

      if (Options["UCI_ShowWDL"])
          ss << UCI::wdl(v, pos.game_ply());

      if (i == pvIdx && !tb && updated) // tablebase- and previous-scores are exact
         ss << (rootMoves[i].scoreLowerbound ? " lowerbound" : (rootMoves[i].scoreUpperbound ? " upperbound" : ""));

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed
         << " hashfull " << TT.hashfull()
         << " tbhits "   << tbHits
         << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

void Tablebases::rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {

    RootInTB = false;
    UseRule50 = bool(Options["Syzygy50MoveRule"]);
    Cardinality = int(Options["SyzygyProbeLimit"]);

    if (Cardinality > MaxCardinality)
        Cardinality = MaxCardinality;

    if (Cardinality >= popcount(pos.pieces()) && !pos.can_castle(ANY_CASTLING))
    {
        // Rank moves using DTZ tables
        RootInTB = root_probe(pos, rootMoves);

        if (!RootInTB)
        {
            // DTZ tables are missing; try to rank moves using WDL tables
            RootInTB = root_probe_wdl(pos, rootMoves);
        }
    }

    if (RootInTB)
    {
        // Sort moves according to TB rank
        std::stable_sort(rootMoves.begin(), rootMoves.end(),
                  [](const RootMove &a, const RootMove &b) { return a.tbRank > b.tbRank; } );
    }
    else
    {
        // Clean up if root_probe() and root_probe_wdl() have failed
        for (auto& m : rootMoves)
            m.tbRank = 0;
    }
}

} // namespace Stockfish