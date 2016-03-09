// decoder/lattice-faster-decoder.cc

// Copyright 2009-2012  Microsoft Corporation  Mirko Hannemann
//           2013-2014  Johns Hopkins University (Author: Daniel Povey)
//                2014  Guoguo Chen

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

// Note on svn: this file is "upstream" from lattice-faster-online-decoder.cc,
// and
// changes in this file should be merged into lattice-faster-online-decoder.cc,
// after committing the changes to this file, using the command
// svn merge ^/sandbox/online/src/decoder/lattice-faster-decoder.cc
// lattice-faster-online-decoder.cc

#include "decoder/lattice-faster-decoder.h"
#include "lat/lattice-functions.h"

namespace kaldi {

// instantiate this class once for each thing you have to decode.
LatticeFasterDecoder::LatticeFasterDecoder(
    const fst::Fst<fst::StdArc> &fst, const LatticeFasterDecoderConfig &config)
    : fst_(fst), delete_fst_(false), config_(config), num_toks_(0) {
  config.Check();
  toks_.SetSize(
      1000);  // just so on the first frame we do something reasonable.
}

LatticeFasterDecoder::LatticeFasterDecoder(
    const LatticeFasterDecoderConfig &config, fst::Fst<fst::StdArc> *fst)
    : fst_(*fst), delete_fst_(true), config_(config), num_toks_(0) {
  config.Check();
  toks_.SetSize(
      1000);  // just so on the first frame we do something reasonable.
}

LatticeFasterDecoder::~LatticeFasterDecoder() {
  DeleteElems(toks_.Clear());
  ClearActiveTokens();
  if (delete_fst_) delete &(fst_);
}

void LatticeFasterDecoder::InitDecoding() {
  // clean up from last time:
  DeleteElems(toks_.Clear());
  cost_offsets_.clear();
  ClearActiveTokens();
  warned_ = false;
  num_toks_ = 0;
  decoding_finalized_ = false;
  final_costs_.clear();
  StateId start_state = fst_.Start();
  KALDI_ASSERT(start_state != fst::kNoStateId);
  active_toks_.resize(1);
  Token *start_tok = new Token(0.0, 0.0, NULL, NULL);
  active_toks_[0].toks = start_tok;
  toks_.Insert(start_state, start_tok);
  num_toks_++;
  ProcessNonemitting();
}

// Returns true if any kind of traceback is available (not necessarily from
// a final state).  It should only very rarely return false; this indicates
// an unusual search error.
bool LatticeFasterDecoder::Decode(DecodableInterface *decodable) {
  InitDecoding();

  // We use 1-based indexing for frames in this decoder (if you view it in
  // terms of features), but note that the decodable object uses zero-based
  // numbering, which we have to correct for when we call it.

  while (!decodable->IsLastFrame(NumFramesDecoded() - 1)) {
    if (NumFramesDecoded() % config_.prune_interval == 0)
      PruneActiveTokens(config_.lattice_beam * config_.prune_scale);
    ProcessEmitting(decodable);  // Note: the value returned by
                                 // NumFramesDecoded() is incremented by
                                 // ProcessEmitting().
    ProcessNonemitting();
  }
  FinalizeDecoding();

  // Returns true if we have any kind of traceback available (not necessarily
  // to the end state; query ReachedFinal() for that).
  return !active_toks_.empty() && active_toks_.back().toks != NULL;
}

// Outputs an FST corresponding to the single best path through the lattice.
bool LatticeFasterDecoder::GetBestPath(Lattice *olat,
                                       bool use_final_probs) const {
  Lattice raw_lat;
  GetRawLattice(&raw_lat, use_final_probs);
  ShortestPath(raw_lat, olat);
  return (olat->NumStates() != 0);
}

// Outputs an FST corresponding to the raw, state-level
// tracebacks.
bool LatticeFasterDecoder::GetRawLattice(Lattice *ofst,
                                         bool use_final_probs) const {
  typedef LatticeArc Arc;
  typedef Arc::StateId StateId;
  typedef Arc::Weight Weight;
  typedef Arc::Label Label;

  // Note: you can't use the old interface (Decode()) if you want to
  // get the lattice with use_final_probs = false.  You'd have to do
  // InitDecoding() and then AdvanceDecoding().
  if (decoding_finalized_ && !use_final_probs)
    KALDI_ERR << "You cannot call FinalizeDecoding() and then call "
              << "GetRawLattice() with use_final_probs == false";

  unordered_map<Token *, BaseFloat> final_costs_local;

  const unordered_map<Token *, BaseFloat> &final_costs =
      (decoding_finalized_ ? final_costs_ : final_costs_local);
  if (!decoding_finalized_ && use_final_probs)
    ComputeFinalCosts(&final_costs_local, NULL, NULL);

  ofst->DeleteStates();
  // num-frames plus one (since frames are one-based, and we have
  // an extra frame for the start-state).
  int32 num_frames = active_toks_.size() - 1;
  KALDI_ASSERT(num_frames > 0);
  const int32 bucket_count = num_toks_ / 2 + 3;
  unordered_map<Token *, StateId> tok_map(bucket_count);
  // First create all states.
  std::vector<Token *> token_list;
  for (int32 f = 0; f <= num_frames; f++) {
    if (active_toks_[f].toks == NULL) {
      KALDI_WARN << "GetRawLattice: no tokens active on frame " << f
                 << ": not producing lattice.\n";
      return false;
    }
    TopSortTokens(active_toks_[f].toks, &token_list);
    for (size_t i = 0; i < token_list.size(); i++)
      if (token_list[i] != NULL) tok_map[token_list[i]] = ofst->AddState();
  }
  // The next statement sets the start state of the output FST.  Because we
  // topologically sorted the tokens, state zero must be the start-state.
  ofst->SetStart(0);

  KALDI_VLOG(4) << "init:" << num_toks_ / 2 + 3
                << " buckets:" << tok_map.bucket_count()
                << " load:" << tok_map.load_factor()
                << " max:" << tok_map.max_load_factor();
  // Now create all arcs.
  for (int32 f = 0; f <= num_frames; f++) {
    for (Token *tok = active_toks_[f].toks; tok != NULL; tok = tok->next) {
      StateId cur_state = tok_map[tok];
      for (ForwardLink *l = tok->links; l != NULL; l = l->next) {
        unordered_map<Token *, StateId>::const_iterator iter =
            tok_map.find(l->next_tok);
        StateId nextstate = iter->second;
        KALDI_ASSERT(iter != tok_map.end());
        BaseFloat cost_offset = 0.0;
        if (l->ilabel != 0) {  // emitting..
          KALDI_ASSERT(f >= 0 && f < cost_offsets_.size());
          cost_offset = cost_offsets_[f];
        }
        Arc arc(l->ilabel, l->olabel,
                Weight(l->graph_cost, l->acoustic_cost - cost_offset),
                nextstate);
        ofst->AddArc(cur_state, arc);
      }
      if (f == num_frames) {
        if (use_final_probs && !final_costs.empty()) {
          unordered_map<Token *, BaseFloat>::const_iterator iter =
              final_costs.find(tok);
          if (iter != final_costs.end())
            ofst->SetFinal(cur_state, LatticeWeight(iter->second, 0));
        } else {
          ofst->SetFinal(cur_state, LatticeWeight::One());
        }
      }
    }
  }
  return (ofst->NumStates() > 0);
}

// This function is now deprecated, since now we do determinization from outside
// the LatticeFasterDecoder class.  Outputs an FST corresponding to the
// lattice-determinized lattice (one path per word sequence).
bool LatticeFasterDecoder::GetLattice(CompactLattice *ofst,
                                      bool use_final_probs) const {
  Lattice raw_fst;
  GetRawLattice(&raw_fst, use_final_probs);
  Invert(&raw_fst);  // make it so word labels are on the input.
  // (in phase where we get backward-costs).
  fst::ILabelCompare<LatticeArc> ilabel_comp;
  ArcSort(&raw_fst, ilabel_comp);  // sort on ilabel; makes
  // lattice-determinization more efficient.

  fst::DeterminizeLatticePrunedOptions lat_opts;
  lat_opts.max_mem = config_.det_opts.max_mem;

  DeterminizeLatticePruned(raw_fst, config_.lattice_beam, ofst, lat_opts);
  raw_fst.DeleteStates();  // Free memory-- raw_fst no longer needed.
  Connect(ofst);           // Remove unreachable states... there might be
  // a small number of these, in some cases.
  // Note: if something went wrong and the raw lattice was empty,
  // we should still get to this point in the code without warnings or failures.
  return (ofst->NumStates() != 0);
}

void LatticeFasterDecoder::PossiblyResizeHash(size_t num_toks) {
  size_t new_sz = static_cast<size_t>(static_cast<BaseFloat>(num_toks) *
                                      config_.hash_ratio);
  if (new_sz > toks_.Size()) {
    toks_.SetSize(new_sz);
  }
}

// FindOrAddToken either locates a token in hash of toks_,
// or if necessary inserts a new, empty token (i.e. with no forward links)
// for the current frame.  [note: it's inserted if necessary into hash toks_
// and also into the singly linked list of tokens active on this frame
// (whose head is at active_toks_[frame]).
inline LatticeFasterDecoder::Token *LatticeFasterDecoder::FindOrAddToken(
    StateId state, int32 frame_plus_one, BaseFloat tot_cost, bool *changed) {
  // Returns the Token pointer.  Sets "changed" (if non-NULL) to true
  // if the token was newly created or the cost changed.
  KALDI_ASSERT(frame_plus_one < active_toks_.size());
  Token *&toks = active_toks_[frame_plus_one].toks;
  Elem *e_found = toks_.Find(state);
  if (e_found == NULL) {  // no such token presently.
    const BaseFloat extra_cost = 0.0;
    // tokens on the currently final frame have zero extra_cost
    // as any of them could end up
    // on the winning path.
    Token *new_tok = new Token(tot_cost, extra_cost, NULL, toks);
    // NULL: no forward links yet
    toks = new_tok;
    num_toks_++;
    toks_.Insert(state, new_tok);
    if (changed) *changed = true;
    return new_tok;
  } else {
    Token *tok = e_found->val;  // There is an existing Token for this state.
    if (tok->tot_cost > tot_cost) {  // replace old token
      tok->tot_cost = tot_cost;
      // we don't allocate a new token, the old stays linked in active_toks_
      // we only replace the tot_cost
      // in the current frame, there are no forward links (and no extra_cost)
      // only in ProcessNonemitting we have to delete forward links
      // in case we visit a state for the second time
      // those forward links, that lead to this replaced token before:
      // they remain and will hopefully be pruned later (PruneForwardLinks...)
      if (changed) *changed = true;
    } else {
      if (changed) *changed = false;
    }
    return tok;
  }
}

// prunes outgoing links for all tokens in active_toks_[frame]
// it's called by PruneActiveTokens
// all links, that have link_extra_cost > lattice_beam are pruned
void LatticeFasterDecoder::PruneForwardLinks(int32 frame_plus_one,
                                             bool *extra_costs_changed,
                                             bool *links_pruned,
                                             BaseFloat delta) {
  // delta is the amount by which the extra_costs must change
  // If delta is larger,  we'll tend to go back less far
  //    toward the beginning of the file.
  // extra_costs_changed is set to true if extra_cost was changed for any token
  // links_pruned is set to true if any link in any token was pruned

  *extra_costs_changed = false;
  *links_pruned = false;
  KALDI_ASSERT(frame_plus_one >= 0 && frame_plus_one < active_toks_.size());
  if (active_toks_[frame_plus_one].toks ==
      NULL) {  // empty list; should not happen.
    if (!warned_) {
      KALDI_WARN << "No tokens alive [doing pruning].. warning first "
                    "time only for each utterance\n";
      warned_ = true;
    }
  }

  // We have to iterate until there is no more change, because the links
  // are not guaranteed to be in topological order.
  bool changed = true;  // difference new minus old extra cost >= delta ?
  while (changed) {
    changed = false;
    for (Token *tok = active_toks_[frame_plus_one].toks; tok != NULL;
         tok = tok->next) {
      ForwardLink *link, *prev_link = NULL;
      // will recompute tok_extra_cost for tok.
      BaseFloat tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      // tok_extra_cost is the best (min) of link_extra_cost of outgoing links
      for (link = tok->links; link != NULL;) {
        // See if we need to excise this link...
        Token *next_tok = link->next_tok;
        BaseFloat link_extra_cost =
            next_tok->extra_cost +
            ((tok->tot_cost + link->acoustic_cost + link->graph_cost) -
             next_tok->tot_cost);  // difference in brackets is >= 0
        // link_exta_cost is the difference in score between the best paths
        // through link source state and through link destination state
        KALDI_ASSERT(link_extra_cost == link_extra_cost);  // check for NaN
        if (link_extra_cost > config_.lattice_beam) {      // excise link
          ForwardLink *next_link = link->next;
          if (prev_link != NULL)
            prev_link->next = next_link;
          else
            tok->links = next_link;
          delete link;
          link = next_link;  // advance link but leave prev_link the same.
          *links_pruned = true;
        } else {  // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) {  // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost)
            tok_extra_cost = link_extra_cost;
          prev_link = link;  // move to next link
          link = link->next;
        }
      }  // for all outgoing links
      if (fabs(tok_extra_cost - tok->extra_cost) > delta)
        changed = true;  // difference new minus old is bigger than delta
      tok->extra_cost = tok_extra_cost;
      // will be +infinity or <= lattice_beam_.
      // infinity indicates, that no forward link survived pruning
    }  // for all Token on active_toks_[frame]
    if (changed) *extra_costs_changed = true;

    // Note: it's theoretically possible that aggressive compiler
    // optimizations could cause an infinite loop here for small delta and
    // high-dynamic-range scores.
  }  // while changed
}

// PruneForwardLinksFinal is a version of PruneForwardLinks that we call
// on the final frame.  If there are final tokens active, it uses
// the final-probs for pruning, otherwise it treats all tokens as final.
void LatticeFasterDecoder::PruneForwardLinksFinal() {
  KALDI_ASSERT(!active_toks_.empty());
  int32 frame_plus_one = active_toks_.size() - 1;

  if (active_toks_[frame_plus_one].toks ==
      NULL)  // empty list; should not happen.
    KALDI_WARN << "No tokens alive at end of file";

  typedef unordered_map<Token *, BaseFloat>::const_iterator IterType;
  ComputeFinalCosts(&final_costs_, &final_relative_cost_, &final_best_cost_);
  decoding_finalized_ = true;
  // We call DeleteElems() as a nicety, not because it's really necessary;
  // otherwise there would be a time, after calling PruneTokensForFrame() on the
  // final frame, when toks_.GetList() or toks_.Clear() would contain pointers
  // to nonexistent tokens.
  DeleteElems(toks_.Clear());

  // Now go through tokens on this frame, pruning forward links...  may have to
  // iterate a few times until there is no more change, because the list is not
  // in topological order.  This is a modified version of the code in
  // PruneForwardLinks, but here we also take account of the final-probs.
  bool changed = true;
  BaseFloat delta = 1.0e-05;
  while (changed) {
    changed = false;
    for (Token *tok = active_toks_[frame_plus_one].toks; tok != NULL;
         tok = tok->next) {
      ForwardLink *link, *prev_link = NULL;
      // will recompute tok_extra_cost.  It has a term in it that corresponds
      // to the "final-prob", so instead of initializing tok_extra_cost to
      // infinity
      // below we set it to the difference between the (score+final_prob) of
      // this token,
      // and the best such (score+final_prob).
      BaseFloat final_cost;
      if (final_costs_.empty()) {
        final_cost = 0.0;
      } else {
        IterType iter = final_costs_.find(tok);
        if (iter != final_costs_.end())
          final_cost = iter->second;
        else
          final_cost = std::numeric_limits<BaseFloat>::infinity();
      }
      BaseFloat tok_extra_cost = tok->tot_cost + final_cost - final_best_cost_;
      // tok_extra_cost will be a "min" over either directly being final, or
      // being indirectly final through other links, and the loop below may
      // decrease its value:
      for (link = tok->links; link != NULL;) {
        // See if we need to excise this link...
        Token *next_tok = link->next_tok;
        BaseFloat link_extra_cost =
            next_tok->extra_cost +
            ((tok->tot_cost + link->acoustic_cost + link->graph_cost) -
             next_tok->tot_cost);
        if (link_extra_cost > config_.lattice_beam) {  // excise link
          ForwardLink *next_link = link->next;
          if (prev_link != NULL)
            prev_link->next = next_link;
          else
            tok->links = next_link;
          delete link;
          link = next_link;  // advance link but leave prev_link the same.
        } else {  // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) {  // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost)
            tok_extra_cost = link_extra_cost;
          prev_link = link;
          link = link->next;
        }
      }
      // prune away tokens worse than lattice_beam above best path.  This step
      // was not necessary in the non-final case because then, this case
      // showed up as having no forward links.  Here, the tok_extra_cost has
      // an extra component relating to the final-prob.
      if (tok_extra_cost > config_.lattice_beam)
        tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      // to be pruned in PruneTokensForFrame

      if (!ApproxEqual(tok->extra_cost, tok_extra_cost, delta)) changed = true;
      tok->extra_cost =
          tok_extra_cost;  // will be +infinity or <= lattice_beam_.
    }
  }  // while changed
}

BaseFloat LatticeFasterDecoder::FinalRelativeCost() const {
  if (!decoding_finalized_) {
    BaseFloat relative_cost;
    ComputeFinalCosts(NULL, &relative_cost, NULL);
    return relative_cost;
  } else {
    // we're not allowed to call that function if FinalizeDecoding() has
    // been called; return a cached value.
    return final_relative_cost_;
  }
}

// Prune away any tokens on this frame that have no forward links.
// [we don't do this in PruneForwardLinks because it would give us
// a problem with dangling pointers].
// It's called by PruneActiveTokens if any forward links have been pruned
void LatticeFasterDecoder::PruneTokensForFrame(int32 frame_plus_one) {
  KALDI_ASSERT(frame_plus_one >= 0 && frame_plus_one < active_toks_.size());
  Token *&toks = active_toks_[frame_plus_one].toks;
  if (toks == NULL) KALDI_WARN << "No tokens alive [doing pruning]";
  Token *tok, *next_tok, *prev_tok = NULL;
  for (tok = toks; tok != NULL; tok = next_tok) {
    next_tok = tok->next;
    if (tok->extra_cost == std::numeric_limits<BaseFloat>::infinity()) {
      // token is unreachable from end of graph; (no forward links survived)
      // excise tok from list and delete tok.
      if (prev_tok != NULL)
        prev_tok->next = tok->next;
      else
        toks = tok->next;
      delete tok;
      num_toks_--;
    } else {  // fetch next Token
      prev_tok = tok;
    }
  }
}

// Go backwards through still-alive tokens, pruning them, starting not from
// the current frame (where we want to keep all tokens) but from the frame
// before
// that.  We go backwards through the frames and stop when we reach a point
// where the delta-costs are not changing (and the delta controls when we
// consider
// a cost to have "not changed").
void LatticeFasterDecoder::PruneActiveTokens(BaseFloat delta) {
  int32 cur_frame_plus_one = NumFramesDecoded();
  int32 num_toks_begin = num_toks_;
  // The index "f" below represents a "frame plus one", i.e. you'd have to
  // subtract
  // one to get the corresponding index for the decodable object.
  for (int32 f = cur_frame_plus_one - 1; f >= 0; f--) {
    // Reason why we need to prune forward links in this situation:
    // (1) we have never pruned them (new TokenList)
    // (2) we have not yet pruned the forward links to the next f,
    // after any of those tokens have changed their extra_cost.
    if (active_toks_[f].must_prune_forward_links) {
      bool extra_costs_changed = false, links_pruned = false;
      PruneForwardLinks(f, &extra_costs_changed, &links_pruned, delta);
      if (extra_costs_changed && f > 0)  // any token has changed extra_cost
        active_toks_[f - 1].must_prune_forward_links = true;
      if (links_pruned)  // any link was pruned
        active_toks_[f].must_prune_tokens = true;
      active_toks_[f].must_prune_forward_links = false;  // job done
    }
    if (f + 1 < cur_frame_plus_one &&  // except for last f (no forward links)
        active_toks_[f + 1].must_prune_tokens) {
      PruneTokensForFrame(f + 1);
      active_toks_[f + 1].must_prune_tokens = false;
    }
  }
  KALDI_VLOG(4) << "PruneActiveTokens: pruned tokens from " << num_toks_begin
                << " to " << num_toks_;
}

void LatticeFasterDecoder::ComputeFinalCosts(
    unordered_map<Token *, BaseFloat> *final_costs,
    BaseFloat *final_relative_cost, BaseFloat *final_best_cost) const {
  KALDI_ASSERT(!decoding_finalized_);
  if (final_costs != NULL) final_costs->clear();
  const Elem *final_toks = toks_.GetList();
  BaseFloat infinity = std::numeric_limits<BaseFloat>::infinity();
  BaseFloat best_cost = infinity, best_cost_with_final = infinity;
  while (final_toks != NULL) {
    StateId state = final_toks->key;
    Token *tok = final_toks->val;
    const Elem *next = final_toks->tail;
    BaseFloat final_cost = fst_.Final(state).Value();
    BaseFloat cost = tok->tot_cost, cost_with_final = cost + final_cost;
    best_cost = std::min(cost, best_cost);
    best_cost_with_final = std::min(cost_with_final, best_cost_with_final);
    if (final_costs != NULL && final_cost != infinity)
      (*final_costs)[tok] = final_cost;
    final_toks = next;
  }
  if (final_relative_cost != NULL) {
    if (best_cost == infinity && best_cost_with_final == infinity) {
      // Likely this will only happen if there are no tokens surviving.
      // This seems the least bad way to handle it.
      *final_relative_cost = infinity;
    } else {
      *final_relative_cost = best_cost_with_final - best_cost;
    }
  }
  if (final_best_cost != NULL) {
    if (best_cost_with_final != infinity) {  // final-state exists.
      *final_best_cost = best_cost_with_final;
    } else {  // no final-state exists.
      *final_best_cost = best_cost;
    }
  }
}

void LatticeFasterDecoder::AdvanceDecoding(DecodableInterface *decodable,
                                           int32 max_num_frames) {
  KALDI_ASSERT(!active_toks_.empty() && !decoding_finalized_ &&
               "You must call InitDecoding() before AdvanceDecoding");
  int32 num_frames_ready = decodable->NumFramesReady();
  // num_frames_ready must be >= num_frames_decoded, or else
  // the number of frames ready must have decreased (which doesn't
  // make sense) or the decodable object changed between calls
  // (which isn't allowed).
  KALDI_ASSERT(num_frames_ready >= NumFramesDecoded());
  int32 target_frames_decoded = num_frames_ready;
  if (max_num_frames >= 0)
    target_frames_decoded =
        std::min(target_frames_decoded, NumFramesDecoded() + max_num_frames);
  while (NumFramesDecoded() < target_frames_decoded) {
    if (NumFramesDecoded() % config_.prune_interval == 0) {
      PruneActiveTokens(config_.lattice_beam * config_.prune_scale);
    }
    // note: ProcessEmitting() increments NumFramesDecoded().
    ProcessEmitting(decodable);
    ProcessNonemitting();
  }
}

// FinalizeDecoding() is a version of PruneActiveTokens that we call
// (optionally) on the final frame.  Takes into account the final-prob of
// tokens.  This function used to be called PruneActiveTokensFinal().
void LatticeFasterDecoder::FinalizeDecoding() {
  int32 final_frame_plus_one = NumFramesDecoded();
  int32 num_toks_begin = num_toks_;
  // PruneForwardLinksFinal() prunes final frame (with final-probs), and
  // sets decoding_finalized_.
  PruneForwardLinksFinal();
  for (int32 f = final_frame_plus_one - 1; f >= 0; f--) {
    bool b1, b2;               // values not used.
    BaseFloat dontcare = 0.0;  // delta of zero means we must always update
    PruneForwardLinks(f, &b1, &b2, dontcare);
    PruneTokensForFrame(f + 1);
  }
  PruneTokensForFrame(0);
  KALDI_VLOG(4) << "pruned tokens from " << num_toks_begin << " to "
                << num_toks_;
}

/// Gets the weight cutoff.  Also counts the active tokens.
BaseFloat LatticeFasterDecoder::GetCutoff(Elem *list_head, size_t *tok_count,
                                          BaseFloat *adaptive_beam,
                                          Elem **best_elem) {
  BaseFloat best_weight = std::numeric_limits<BaseFloat>::infinity();
  // positive == high cost == bad.
  size_t count = 0;
  if (config_.max_active == std::numeric_limits<int32>::max() &&
      config_.min_active == 0) {
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = static_cast<BaseFloat>(e->val->tot_cost);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;
    if (adaptive_beam != NULL) *adaptive_beam = config_.beam;
    return best_weight + config_.beam;
  } else {
    tmp_array_.clear();
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = e->val->tot_cost;
      tmp_array_.push_back(w);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;

    BaseFloat beam_cutoff = best_weight + config_.beam,
              min_active_cutoff = std::numeric_limits<BaseFloat>::infinity(),
              max_active_cutoff = std::numeric_limits<BaseFloat>::infinity();

    if (tmp_array_.size() > static_cast<size_t>(config_.max_active)) {
      std::nth_element(tmp_array_.begin(),
                       tmp_array_.begin() + config_.max_active,
                       tmp_array_.end());
      max_active_cutoff = tmp_array_[config_.max_active];
    }
    if (tmp_array_.size() > static_cast<size_t>(config_.min_active)) {
      if (config_.min_active == 0)
        min_active_cutoff = best_weight;
      else {
        std::nth_element(
            tmp_array_.begin(), tmp_array_.begin() + config_.min_active,
            tmp_array_.size() > static_cast<size_t>(config_.max_active)
                ? tmp_array_.begin() + config_.max_active
                : tmp_array_.end());
        min_active_cutoff = tmp_array_[config_.min_active];
      }
    }

    if (max_active_cutoff < beam_cutoff) {  // max_active is tighter than beam.
      if (adaptive_beam)
        *adaptive_beam = max_active_cutoff - best_weight + config_.beam_delta;
      return max_active_cutoff;
    } else if (min_active_cutoff >
               beam_cutoff) {  // min_active is looser than beam.
      if (adaptive_beam)
        *adaptive_beam = min_active_cutoff - best_weight + config_.beam_delta;
      return min_active_cutoff;
    } else {
      *adaptive_beam = config_.beam;
      return beam_cutoff;
    }
  }
}

void LatticeFasterDecoder::ProcessEmitting(DecodableInterface *decodable) {
  KALDI_ASSERT(active_toks_.size() > 0);
  int32 frame = active_toks_.size() - 1;  // frame is the frame-index
  // (zero-based) used to get likelihoods
  // from the decodable object.
  active_toks_.resize(active_toks_.size() + 1);

  Elem *final_toks =
      toks_.Clear();  // analogous to swapping prev_toks_ / cur_toks_
                      // in simple-decoder.h.   Removes the Elems from
                      // being indexed in the hash in toks_.
  Elem *best_elem = NULL;
  BaseFloat adaptive_beam;
  size_t tok_cnt;
  BaseFloat cur_cutoff =
      GetCutoff(final_toks, &tok_cnt, &adaptive_beam, &best_elem);
  PossiblyResizeHash(
      tok_cnt);  // This makes sure the hash is always big enough.

  BaseFloat next_cutoff = std::numeric_limits<BaseFloat>::infinity();
  // pruning "online" before having seen all tokens

  BaseFloat cost_offset = 0.0;  // Used to keep probabilities in a good
  // dynamic range.

  // First process the best token to get a hopefully
  // reasonably tight bound on the next cutoff.  The only
  // products of the next block are "next_cutoff" and "cost_offset".
  if (best_elem) {
    StateId state = best_elem->key;
    Token *tok = best_elem->val;
    cost_offset = -tok->tot_cost;
    for (fst::ArcIterator<fst::Fst<Arc> > aiter(fst_, state); !aiter.Done();
         aiter.Next()) {
      Arc arc = aiter.Value();
      if (arc.ilabel != 0) {  // propagate..
        arc.weight = Times(
            arc.weight,
            Weight(cost_offset - decodable->LogLikelihood(frame, arc.ilabel)));
        BaseFloat new_weight = arc.weight.Value() + tok->tot_cost;
        if (new_weight + adaptive_beam < next_cutoff)
          next_cutoff = new_weight + adaptive_beam;
      }
    }
  }

  // Store the offset on the acoustic likelihoods that we're applying.
  // Could just do cost_offsets_.push_back(cost_offset), but we
  // do it this way as it's more robust to future code changes.
  cost_offsets_.resize(frame + 1, 0.0);
  cost_offsets_[frame] = cost_offset;

  // the tokens are now owned here, in final_toks, and the hash is empty.
  // 'owned' is a complex thing here; the point is we need to call DeleteElem
  // on each elem 'e' to let toks_ know we're done with them.
  for (Elem *e = final_toks, *e_tail; e != NULL; e = e_tail) {
    // loop this way because we delete "e" as we go.
    StateId state = e->key;
    Token *tok = e->val;
    if (tok->tot_cost <= cur_cutoff) {
      for (fst::ArcIterator<fst::Fst<Arc> > aiter(fst_, state); !aiter.Done();
           aiter.Next()) {
        const Arc &arc = aiter.Value();
        if (arc.ilabel != 0) {  // propagate..
          BaseFloat ac_cost = cost_offset -
                              decodable->LogLikelihood(frame, arc.ilabel),
                    graph_cost = arc.weight.Value(), cur_cost = tok->tot_cost,
                    tot_cost = cur_cost + ac_cost + graph_cost;
          if (tot_cost > next_cutoff)
            continue;
          else if (tot_cost + config_.beam < next_cutoff)
            next_cutoff =
                tot_cost + config_.beam;  // prune by best current token
          // Note: the frame indexes into active_toks_ are one-based,
          // hence the + 1.
          Token *next_tok =
              FindOrAddToken(arc.nextstate, frame + 1, tot_cost, NULL);
          // NULL: no change indicator needed

          // Add ForwardLink from tok to next_tok (put on head of list
          // tok->links)
          tok->links = new ForwardLink(next_tok, arc.ilabel, arc.olabel,
                                       graph_cost, ac_cost, tok->links);
        }
      }  // for all arcs
    }
    e_tail = e->tail;
    toks_.Delete(e);  // delete Elem
  }
}

// TODO: could possibly add adaptive_beam back as an argument here (was
// returned from ProcessEmitting, in faster-decoder.h).
void LatticeFasterDecoder::ProcessNonemitting() {
  KALDI_ASSERT(!active_toks_.empty());
  int32 frame = static_cast<int32>(active_toks_.size()) - 2;
  // Note: "frame" is the time-index we just processed, or -1 if
  // we are processing the nonemitting transitions before the
  // first frame (called from InitDecoding()).

  // Processes nonemitting arcs for one frame.  Propagates within toks_.
  // Note-- this queue structure is is not very optimal as
  // it may cause us to process states unnecessarily (e.g. more than once),
  // but in the baseline code, turning this vector into a set to fix this
  // problem did not improve overall speed.

  KALDI_ASSERT(queue_.empty());
  BaseFloat best_cost = std::numeric_limits<BaseFloat>::infinity();
  for (const Elem *e = toks_.GetList(); e != NULL; e = e->tail) {
    queue_.push_back(e->key);
    // for pruning with current best token
    best_cost = std::min(best_cost, static_cast<BaseFloat>(e->val->tot_cost));
  }
  if (queue_.empty()) {
    if (!warned_) {
      KALDI_WARN << "Error, no surviving tokens: frame is " << frame;
      warned_ = true;
    }
  }
  BaseFloat cutoff = best_cost + config_.beam;

  while (!queue_.empty()) {
    StateId state = queue_.back();
    queue_.pop_back();

    Token *tok = toks_.Find(state)->val;  // would segfault if state not in
                                          // toks_ but this can't happen.
    BaseFloat cur_cost = tok->tot_cost;
    if (cur_cost > cutoff)  // Don't bother processing successors.
      continue;
    // If "tok" has any existing forward links, delete them,
    // because we're about to regenerate them.  This is a kind
    // of non-optimality (remember, this is the simple decoder),
    // but since most states are emitting it's not a huge issue.
    tok->DeleteForwardLinks();  // necessary when re-visiting
    tok->links = NULL;
    for (fst::ArcIterator<fst::Fst<Arc> > aiter(fst_, state); !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      if (arc.ilabel == 0) {  // propagate nonemitting only...
        BaseFloat graph_cost = arc.weight.Value(),
                  tot_cost = cur_cost + graph_cost;
        if (tot_cost < cutoff) {
          bool changed;

          Token *new_tok =
              FindOrAddToken(arc.nextstate, frame + 1, tot_cost, &changed);

          tok->links = new ForwardLink(new_tok, 0, arc.olabel, graph_cost, 0,
                                       tok->links);

          // "changed" tells us whether the new token has a different
          // cost from before, or is new [if so, add into queue].
          if (changed) queue_.push_back(arc.nextstate);
        }
      }
    }  // for all arcs
  }    // while queue not empty
}

void LatticeFasterDecoder::DeleteElems(Elem *list) {
  for (Elem *e = list, *e_tail; e != NULL; e = e_tail) {
    e_tail = e->tail;
    toks_.Delete(e);
  }
}

void LatticeFasterDecoder::ClearActiveTokens() {  // a cleanup routine, at utt
                                                  // end/begin
  for (size_t i = 0; i < active_toks_.size(); i++) {
    // Delete all tokens alive on this frame, and any forward
    // links they may have.
    for (Token *tok = active_toks_[i].toks; tok != NULL;) {
      tok->DeleteForwardLinks();
      Token *next_tok = tok->next;
      delete tok;
      num_toks_--;
      tok = next_tok;
    }
  }
  active_toks_.clear();
  KALDI_ASSERT(num_toks_ == 0);
}

// static
void LatticeFasterDecoder::TopSortTokens(Token *tok_list,
                                         std::vector<Token *> *topsorted_list) {
  unordered_map<Token *, int32> token2pos;
  typedef unordered_map<Token *, int32>::iterator IterType;
  int32 num_toks = 0;
  for (Token *tok = tok_list; tok != NULL; tok = tok->next) num_toks++;
  int32 cur_pos = 0;
  // We assign the tokens numbers num_toks - 1, ... , 2, 1, 0.
  // This is likely to be in closer to topological order than
  // if we had given them ascending order, because of the way
  // new tokens are put at the front of the list.
  for (Token *tok = tok_list; tok != NULL; tok = tok->next)
    token2pos[tok] = num_toks - ++cur_pos;

  unordered_set<Token *> reprocess;

  for (IterType iter = token2pos.begin(); iter != token2pos.end(); ++iter) {
    Token *tok = iter->first;
    int32 pos = iter->second;
    for (ForwardLink *link = tok->links; link != NULL; link = link->next) {
      if (link->ilabel == 0) {
        // We only need to consider epsilon links, since non-epsilon links
        // transition between frames and this function only needs to sort a list
        // of tokens from a single frame.
        IterType following_iter = token2pos.find(link->next_tok);
        if (following_iter !=
            token2pos.end()) {  // another token on this frame,
                                // so must consider it.
          int32 next_pos = following_iter->second;
          if (next_pos < pos) {  // reassign the position of the next Token.
            following_iter->second = cur_pos++;
            reprocess.insert(link->next_tok);
          }
        }
      }
    }
    // In case we had previously assigned this token to be reprocessed, we can
    // erase it from that set because it's "happy now" (we just processed it).
    reprocess.erase(tok);
  }

  size_t max_loop = 1000000,
         loop_count;  // max_loop is to detect epsilon cycles.
  for (loop_count = 0; !reprocess.empty() && loop_count < max_loop;
       ++loop_count) {
    std::vector<Token *> reprocess_vec;
    for (unordered_set<Token *>::iterator iter = reprocess.begin();
         iter != reprocess.end(); ++iter)
      reprocess_vec.push_back(*iter);
    reprocess.clear();
    for (std::vector<Token *>::iterator iter = reprocess_vec.begin();
         iter != reprocess_vec.end(); ++iter) {
      Token *tok = *iter;
      int32 pos = token2pos[tok];
      // Repeat the processing we did above (for comments, see above).
      for (ForwardLink *link = tok->links; link != NULL; link = link->next) {
        if (link->ilabel == 0) {
          IterType following_iter = token2pos.find(link->next_tok);
          if (following_iter != token2pos.end()) {
            int32 next_pos = following_iter->second;
            if (next_pos < pos) {
              following_iter->second = cur_pos++;
              reprocess.insert(link->next_tok);
            }
          }
        }
      }
    }
  }
  KALDI_ASSERT(loop_count < max_loop &&
               "Epsilon loops exist in your decoding "
               "graph (this is not allowed!)");

  topsorted_list->clear();
  topsorted_list->resize(cur_pos,
                         NULL);  // create a list with NULLs in between.
  for (IterType iter = token2pos.begin(); iter != token2pos.end(); ++iter)
    (*topsorted_list)[iter->second] = iter->first;
}

DecodeUtteranceLatticeFasterClass::DecodeUtteranceLatticeFasterClass(
    LatticeFasterDecoder *decoder, DecodableInterface *decodable,
    const TransitionModel &trans_model, const fst::SymbolTable *word_syms,
    std::string utt, BaseFloat acoustic_scale, bool determinize,
    bool allow_partial, Int32VectorWriter *alignments_writer,
    Int32VectorWriter *words_writer,
    CompactLatticeWriter *compact_lattice_writer, LatticeWriter *lattice_writer,
    double *like_sum,  // on success, adds likelihood to this.
    int64 *frame_sum,  // on success, adds #frames to this.
    int32 *num_done,  // on success (including partial decode), increments this.
    int32 *num_err,   // on failure, increments this.
    int32 *num_partial)
    :  // If partial decode (final-state not reached), increments this.
      decoder_(decoder),
      decodable_(decodable),
      trans_model_(&trans_model),
      word_syms_(word_syms),
      utt_(utt),
      acoustic_scale_(acoustic_scale),
      determinize_(determinize),
      allow_partial_(allow_partial),
      alignments_writer_(alignments_writer),
      words_writer_(words_writer),
      compact_lattice_writer_(compact_lattice_writer),
      lattice_writer_(lattice_writer),
      like_sum_(like_sum),
      frame_sum_(frame_sum),
      num_done_(num_done),
      num_err_(num_err),
      num_partial_(num_partial),
      computed_(false),
      success_(false),
      partial_(false),
      clat_(NULL),
      lat_(NULL) {}

void DecodeUtteranceLatticeFasterClass::operator()() {
  // Decoding and lattice determinization happens here.
  computed_ = true;  // Just means this function was called-- a check on the
  // calling code.
  success_ = true;
  using fst::VectorFst;
  if (!decoder_->Decode(decodable_)) {
    KALDI_WARN << "Failed to decode file " << utt_;
    success_ = false;
  }
  if (!decoder_->ReachedFinal()) {
    if (allow_partial_) {
      KALDI_WARN << "Outputting partial output for utterance " << utt_
                 << " since no final-state reached\n";
      partial_ = true;
    } else {
      KALDI_WARN << "Not producing output for utterance " << utt_
                 << " since no final-state reached and "
                 << "--allow-partial=false.\n";
      success_ = false;
    }
  }
  if (!success_) return;

  // Get lattice, and do determinization if requested.
  lat_ = new Lattice;
  decoder_->GetRawLattice(lat_);
  if (lat_->NumStates() == 0)
    KALDI_ERR << "Unexpected problem getting lattice for utterance " << utt_;
  fst::Connect(lat_);
  if (determinize_) {
    clat_ = new CompactLattice;
    if (!DeterminizeLatticePhonePrunedWrapper(
            *trans_model_, lat_, decoder_->GetOptions().lattice_beam, clat_,
            decoder_->GetOptions().det_opts))
      KALDI_WARN << "Determinization finished earlier than the beam for "
                 << "utterance " << utt_;
    delete lat_;
    lat_ = NULL;
    // We'll write the lattice without acoustic scaling.
    if (acoustic_scale_ != 0.0)
      fst::ScaleLattice(fst::AcousticLatticeScale(1.0 / acoustic_scale_),
                        clat_);
  } else {
    // We'll write the lattice without acoustic scaling.
    if (acoustic_scale_ != 0.0)
      fst::ScaleLattice(fst::AcousticLatticeScale(1.0 / acoustic_scale_), lat_);
  }
}

DecodeUtteranceLatticeFasterClass::~DecodeUtteranceLatticeFasterClass() {
  if (!computed_)
    KALDI_ERR
        << "Destructor called without operator (), error in calling code.";

  if (!success_) {
    if (num_err_ != NULL) (*num_err_)++;
  } else {  // successful decode.
    // Getting the one-best output is lightweight enough that we can do it in
    // the destructor (easier than adding more variables to the class, and
    // will rarely slow down the main thread.)
    double likelihood;
    LatticeWeight weight;
    int32 num_frames;
    {  // First do some stuff with word-level traceback...
      // This is basically for diagnostics.
      fst::VectorFst<LatticeArc> decoded;
      decoder_->GetBestPath(&decoded);
      if (decoded.NumStates() == 0) {
        // Shouldn't really reach this point as already checked success.
        KALDI_ERR << "Failed to get traceback for utterance " << utt_;
      }
      std::vector<int32> alignment;
      std::vector<int32> words;
      GetLinearSymbolSequence(decoded, &alignment, &words, &weight);
      num_frames = alignment.size();
      if (words_writer_->IsOpen()) words_writer_->Write(utt_, words);
      if (alignments_writer_->IsOpen())
        alignments_writer_->Write(utt_, alignment);
      if (word_syms_ != NULL) {
        std::cerr << utt_ << ' ';
        for (size_t i = 0; i < words.size(); i++) {
          std::string s = word_syms_->Find(words[i]);
          if (s == "")
            KALDI_ERR << "Word-id " << words[i] << " not in symbol table.";
          std::cerr << s << ' ';
        }
        std::cerr << '\n';
      }
      likelihood = -(weight.Value1() + weight.Value2());
    }

    // Ouptut the lattices.
    if (determinize_) {  // CompactLattice output.
      KALDI_ASSERT(compact_lattice_writer_ != NULL && clat_ != NULL);
      if (clat_->NumStates() == 0) {
        KALDI_WARN << "Empty lattice for utterance " << utt_;
      } else {
        compact_lattice_writer_->Write(utt_, *clat_);
      }
      delete clat_;
      clat_ = NULL;
    } else {
      KALDI_ASSERT(lattice_writer_ != NULL && lat_ != NULL);
      if (lat_->NumStates() == 0) {
        KALDI_WARN << "Empty lattice for utterance " << utt_;
      } else {
        lattice_writer_->Write(utt_, *lat_);
      }
      delete lat_;
      lat_ = NULL;
    }

    // Print out logging information.
    KALDI_LOG << "Log-like per frame for utterance " << utt_ << " is "
              << (likelihood / num_frames) << " over " << num_frames
              << " frames.";
    KALDI_VLOG(2) << "Cost for utterance " << utt_ << " is " << weight.Value1()
                  << " + " << weight.Value2();

    // Now output the various diagnostic variables.
    if (like_sum_ != NULL) *like_sum_ += likelihood;
    if (frame_sum_ != NULL) *frame_sum_ += num_frames;
    if (num_done_ != NULL) (*num_done_)++;
    if (partial_ && num_partial_ != NULL) (*num_partial_)++;
  }
  // We were given ownership of these two objects that were passed in in
  // the initializer.
  delete decoder_;
  delete decodable_;
}

// Takes care of output.  Returns true on success.
bool DecodeUtteranceLatticeFaster(
    LatticeFasterDecoder &decoder,  // not const but is really an input.
    DecodableInterface &decodable,  // not const but is really an input.
    const TransitionModel &trans_model, const fst::SymbolTable *word_syms,
    std::string utt, double acoustic_scale, bool determinize,
    bool allow_partial, Int32VectorWriter *alignment_writer,
    Int32VectorWriter *words_writer,
    CompactLatticeWriter *compact_lattice_writer, LatticeWriter *lattice_writer,
    double *like_ptr) {  // puts utterance's like in like_ptr on success.
  using fst::VectorFst;

  if (!decoder.Decode(&decodable)) {
    KALDI_WARN << "Failed to decode file " << utt;
    return false;
  }
  if (!decoder.ReachedFinal()) {
    if (allow_partial) {
      KALDI_WARN << "Outputting partial output for utterance " << utt
                 << " since no final-state reached\n";
    } else {
      KALDI_WARN << "Not producing output for utterance " << utt
                 << " since no final-state reached and "
                 << "--allow-partial=false.\n";
      return false;
    }
  }

  double likelihood;
  LatticeWeight weight;
  int32 num_frames;
  {  // First do some stuff with word-level traceback...
    VectorFst<LatticeArc> decoded;
    if (!decoder.GetBestPath(&decoded))
      // Shouldn't really reach this point as already checked success.
      KALDI_ERR << "Failed to get traceback for utterance " << utt;

    std::vector<int32> alignment;
    std::vector<int32> words;
    GetLinearSymbolSequence(decoded, &alignment, &words, &weight);
    num_frames = alignment.size();
    if (words_writer->IsOpen()) words_writer->Write(utt, words);
    if (alignment_writer->IsOpen()) alignment_writer->Write(utt, alignment);
    if (word_syms != NULL) {
      std::cerr << utt << ' ';
      for (size_t i = 0; i < words.size(); i++) {
        std::string s = word_syms->Find(words[i]);
        if (s == "")
          KALDI_ERR << "Word-id " << words[i] << " not in symbol table.";
        std::cerr << s << ' ';
      }
      std::cerr << '\n';
    }
    likelihood = -(weight.Value1() + weight.Value2());
  }

  // Get lattice, and do determinization if requested.
  Lattice lat;
  decoder.GetRawLattice(&lat);
  if (lat.NumStates() == 0)
    KALDI_ERR << "Unexpected problem getting lattice for utterance " << utt;
  fst::Connect(&lat);
  if (determinize) {
    CompactLattice clat;
    if (!DeterminizeLatticePhonePrunedWrapper(
            trans_model, &lat, decoder.GetOptions().lattice_beam, &clat,
            decoder.GetOptions().det_opts))
      KALDI_WARN << "Determinization finished earlier than the beam for "
                 << "utterance " << utt;
    // We'll write the lattice without acoustic scaling.
    if (acoustic_scale != 0.0)
      fst::ScaleLattice(fst::AcousticLatticeScale(1.0 / acoustic_scale), &clat);
    compact_lattice_writer->Write(utt, clat);
  } else {
    // We'll write the lattice without acoustic scaling.
    if (acoustic_scale != 0.0)
      fst::ScaleLattice(fst::AcousticLatticeScale(1.0 / acoustic_scale), &lat);
    lattice_writer->Write(utt, lat);
  }
  KALDI_LOG << "Log-like per frame for utterance " << utt << " is "
            << (likelihood / num_frames) << " over " << num_frames
            << " frames.";
  KALDI_VLOG(2) << "Cost for utterance " << utt << " is " << weight.Value1()
                << " + " << weight.Value2();
  *like_ptr = likelihood;
  return true;
}

}  // end namespace kaldi.