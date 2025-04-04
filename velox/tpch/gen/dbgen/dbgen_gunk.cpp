/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Copyright owned by the Transaction Processing Performance Council.
 *
 * A copy of the license is included under extension/tpch/dbgen/LICENSE
 * in this repository.
 *
 * You may not use this file except in compliance with the License.
 *
 * THE TPC SOFTWARE IS AVAILABLE WITHOUT CHARGE FROM TPC.
 */
#include "dbgen/dbgen_gunk.hpp" // @manual

#include "dbgen/dss.h" // @manual

namespace facebook::velox::tpch::dbgen {

void load_dists(long textBufferSize, DBGenContext* ctx) {
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "p_cntr", &p_cntr_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "colors", &colors);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "p_types", &p_types_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "nations", &nations);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "regions", &regions);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "o_oprio", &o_priority_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "instruct", &l_instruct_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "smode", &l_smode_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "category", &l_category_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "rflag", &l_rflag_set);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "msegmnt", &c_mseg_set);

  /* load the distributions that contain text generation */
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "nouns", &nouns);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "verbs", &verbs);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "adjectives", &adjectives);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "adverbs", &adverbs);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "auxillaries", &auxillaries);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "terminators", &terminators);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "articles", &articles);
  read_dist(
      tpch_env_config(DIST_TAG, DIST_DFLT), "prepositions", &prepositions);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "grammar", &grammar);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "np", &np);
  read_dist(tpch_env_config(DIST_TAG, DIST_DFLT), "vp", &vp);

  /* populate the text buffer used to generate random text */
  init_text_pool(textBufferSize, ctx);
}

static void cleanup_dist(distribution* target) {
  if (!target) {
    return;
  }
  if (target->list) {
    for (int i = 0; i < target->count; i++) {
      if (target->list[i].text) {
        free(target->list[i].text);
      }
    }
    free(target->list);
  }
  /* Allocated from permute_dist */
  if (target->permute) {
    free(target->permute);
  }
}

void cleanup_dists(void) {
  cleanup_dist(&p_cntr_set);
  cleanup_dist(&colors);
  cleanup_dist(&p_types_set);
  cleanup_dist(&nations);
  cleanup_dist(&regions);
  cleanup_dist(&o_priority_set);
  cleanup_dist(&l_instruct_set);
  cleanup_dist(&l_smode_set);
  cleanup_dist(&l_category_set);
  cleanup_dist(&l_rflag_set);
  cleanup_dist(&c_mseg_set);
  cleanup_dist(&nouns);
  cleanup_dist(&verbs);
  cleanup_dist(&adjectives);
  cleanup_dist(&adverbs);
  cleanup_dist(&auxillaries);
  cleanup_dist(&terminators);
  cleanup_dist(&articles);
  cleanup_dist(&prepositions);
  cleanup_dist(&grammar);
  cleanup_dist(&np);
  cleanup_dist(&vp);

  free_text_pool();
}

} // namespace facebook::velox::tpch::dbgen
