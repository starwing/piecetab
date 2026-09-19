#define AT_FANOUT    4
#define AT_MAX_LEVEL 64 /* AT_FANOUT=4: see docs/max_levels.md */
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_cases.h"
#include "at_tests.h"
#include "tests.h"

/* fanout-4 runner: TEST wrappers for the shared cases in at_cases.h. */

TEST(splice_brute) { assertok(att_splice_brute_ok()); }
TEST(splice_random_model) { assertok(att_splice_random_ok()); }
TEST(lifecycle) { att_case_lifecycle(); }
TEST(param_guards) { att_case_param_guards(); }
TEST(edge_guards) { att_case_edge_guards(); }
TEST(empty_seek) { att_case_empty_seek(); }
TEST(seek_positions) { att_case_seek_positions(); }
TEST(advance_positions) { att_case_advance_positions(); }
TEST(advance_span) { att_case_advance_span(); }
TEST(split_sequence) { att_case_split_sequence(); }
TEST(delete_middle) { att_case_delete_middle(); }
TEST(delete_last) { att_case_delete_last(); }
TEST(delete_all) { att_case_delete_all(); }
TEST(random_mutate) { att_case_random_mutate(); }
TEST(anchor_id_reuse) { att_case_anchor_id_reuse(); }
TEST(anchor_gravities) { att_case_anchor_gravities(); }
TEST(anchor_zero_length) { att_case_anchor_zero_length(); }
TEST(anchor_ns_tag) { att_case_anchor_ns_tag(); }
TEST(anchor_ns_relocate) { att_case_anchor_ns_relocate(); }
TEST(anchor_range_positions) { att_case_anchor_range_positions(); }
TEST(anchor_query) { att_case_anchor_query(); }
TEST(anchor_delete_cleans) { att_case_anchor_delete_cleans(); }
TEST(anchor_lifecycle_ids) { att_case_anchor_lifecycle_ids(); }
TEST(anchor_invalid_params) { att_case_anchor_invalid_params(); }
TEST(splice_gap_delete_insert) { att_case_splice_gap_delete_insert(); }
TEST(splice_edge_gravity_insert) { att_case_splice_edge_gravity_insert(); }
TEST(splice_delete_spanning) { att_case_splice_delete_spanning(); }
TEST(splice_delete_last_insert) { att_case_splice_delete_last_insert(); }
TEST(splice_zero_cross) { att_case_splice_zero_cross(); }
TEST(splice_params) { att_case_splice_params(); }
TEST(anchor_oom_idalloc) { att_case_anchor_oom_idalloc(); }
TEST(anchor_oom_reserve) { att_case_anchor_oom_reserve(); }
TEST(splice_oom_reserve) { att_case_splice_oom_reserve(); }
TEST(del_oom_zero_alloc) { att_case_del_oom_zero_alloc(); }
TEST(splice_noop_beyond_tail_oom) { att_case_splice_noop_beyond_tail_oom(); }
TEST(anchor_tail_delete) { att_case_anchor_tail_delete(); }
TEST(tail_edge_contract) { att_case_tail_edge_contract(); }
TEST(splice_delete_grow_tail) { att_case_splice_delete_grow_tail(); }
TEST(delete_contracts) { att_case_delete_contracts(); }
TEST(checktree_overfull) { att_case_checktree_overfull(); }
TEST(checktree_bad_links) { att_case_checktree_bad_links(); }
TEST(checker_seq_capacity) { att_case_checker_seq_capacity(); }
TEST(insert_edge_last) { att_case_insert_edge_last(); }
TEST(insert_edge_mid) { att_case_insert_edge_mid(); }
TEST(insert_gap_shift_mid) { att_case_insert_gap_shift_mid(); }
TEST(delete_prefix_leaf_rebalance) { att_case_delete_prefix_leaf_rebalance(); }
TEST(edge_side_missing) { att_case_edge_side_missing(); }
TEST(delete_root_collapse) { att_case_delete_root_collapse(); }
TEST(splice_multi_slot_delete) { att_case_splice_multi_slot_delete(); }
TEST(splice_cross_leaf_rebalance) { att_case_splice_cross_leaf_rebalance(); }
TEST(splice_empty_L_merge) { att_case_splice_empty_L_merge(); }
TEST(delete_cross_leaf_merge) { att_case_delete_cross_leaf_merge(); }
TEST(delete_tail_sentinel) { att_case_delete_tail_sentinel(); }
TEST(splice_delete_shared_tail) { att_case_splice_delete_shared_tail(); }
TEST(splice_delete_chain) { att_case_splice_delete_chain(); }
TEST(splice_delete_tail) { att_case_splice_delete_tail(); }
TEST(splice_shift_negative) { att_case_splice_shift_negative(); }
TEST(iter_multiple_edges) { att_case_iter_multiple_edges(); }
TEST(iter_start_first) { att_case_iter_start_first(); }
TEST(iter_start_empty) { att_case_iter_start_empty(); }
TEST(iter_many_edges_cross_leaves) { att_case_iter_many_edges_cross_leaves(); }
TEST(iter_zero_length_once) { att_case_iter_zero_length_once(); }
TEST(iter_gap_and_edge_starts) { att_case_iter_gap_and_edge_starts(); }
TEST(iter_empty_end_idempotent) { att_case_iter_empty_end_idempotent(); }
TEST(iter_ns_filter) { att_case_iter_ns_filter(); }
TEST(iter_plen) { att_case_iter_plen(); }
TEST(iter_params) { att_case_iter_params(); }
TEST(iter_exhaust_restart) { att_case_iter_exhaust_restart(); }
TEST(iter_exhaust_commit) { att_case_iter_exhaust_commit(); }
TEST(splice_after_iter_exhaust) { att_case_splice_after_iter_exhaust(); }
TEST(iter_empty_start) { att_case_iter_empty_start(); }
TEST(iter_after_splice_insert) { att_case_iter_after_splice_insert(); }
TEST(iter_after_splice_delete) { att_case_iter_after_splice_delete(); }
TEST(splice_overflow_del) { att_case_splice_overflow_del(); }
TEST(splice_delete_large_span) { att_case_splice_delete_large_span(); }
TEST(splice_delete_all) { att_case_splice_delete_all(); }
TEST(anchor_remove_missing) { att_case_anchor_remove_missing(); }
TEST(anchor_at_zero_delete) { att_case_anchor_at_zero_delete(); }
TEST(insert_empty_boundary) { att_case_insert_empty_boundary(); }
TEST(deleteedge_pos_guard) { att_case_deleteedge_pos_guard(); }
TEST(splice_ins_overflow) { att_case_splice_ins_overflow(); }
TEST(advance_overflow_guard) { att_case_advance_overflow_guard(); }
TEST(epoch_advance_stale) { att_case_epoch_advance_stale(); }
TEST(epoch_next_stale) { att_case_epoch_next_stale(); }
TEST(epoch_splice_keeps_own) { att_case_epoch_splice_keeps_own(); }
TEST(epoch_multi_cursor) { att_case_epoch_multi_cursor(); }
TEST(epoch_no_mutation) { att_case_epoch_no_mutation(); }
TEST(epoch_reset) { att_case_epoch_reset(); }
TEST(remove_params) { att_case_remove_params(); }
TEST(remove_cursor) { att_case_remove_cursor(); }
TEST(insert_cursor) { att_case_insert_cursor(); }
TEST(append_cursor) { att_case_append_cursor(); }
TEST(splice_atomic_oom) { att_case_splice_atomic_oom(); }
TEST(remove_append_equivalence) { att_case_remove_append_equivalence(); }
TEST(stitch_delete_cross_leaf) { att_case_stitch_delete_cross_leaf(); }
TEST(stitch_delete_multi_level) { att_case_stitch_delete_multi_level(); }
TEST(stitch_delete_tail) { att_case_stitch_delete_tail(); }
TEST(stitch_delete_same_leaf) { att_case_stitch_delete_same_leaf(); }
TEST(stitch_seam_cleared) { att_case_stitch_seam_cleared(); }
TEST(stitch_rt_cleared) { att_case_stitch_rt_cleared(); }
TEST(stitch_random_edit) { att_case_stitch_random_edit(); }
TEST(stitch_delete_boundary) { att_case_stitch_delete_boundary(); }
TEST(stitch_delete_brute) { att_case_stitch_delete_brute(); }
TEST(stitch_firstedge_keep) { att_case_stitch_firstedge_keep(); }
TEST(stitch_tail_seam_anchors) { att_case_stitch_tail_seam_anchors(); }
TEST(purge_insertion) { att_case_purge_insertion(); }
TEST(purge_delanchor) { att_case_purge_delanchor(); }
/* fanout4-only: the forced child split relies on the fanout-4 layout. */
TEST(insert_split_child) { att_case_insert_split_child(); }
/* shared case; also registered by the fanout8 runner. */
TEST(insert_span_split) { att_case_insert_span_split(); }

#include "anchortree_test_fanout4.gen.inc"
