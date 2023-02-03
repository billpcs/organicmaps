#include "search/pre_ranker.hpp"

#include "search/dummy_rank_table.hpp"
#include "search/lazy_centers_table.hpp"
#include "search/pre_ranking_info.hpp"

#include "editor/osm_editor.hpp"

#include "indexer/data_source.hpp"
#include "indexer/mwm_set.hpp"
#include "indexer/rank_table.hpp"

#include "geometry/mercator.hpp"
#include "geometry/nearby_points_sweeper.hpp"

#include "base/stl_helpers.hpp"

#include <algorithm>
#include <iterator>

namespace search
{
using namespace std;

namespace
{
void SweepNearbyResults(m2::PointD const & eps, unordered_set<FeatureID> const & prevEmit,
                        vector<PreRankerResult> & results)
{
  m2::NearbyPointsSweeper sweeper(eps.x, eps.y);
  for (size_t i = 0; i < results.size(); ++i)
  {
    auto const & p = results[i].GetInfo().m_center;
    uint8_t const rank = results[i].GetInfo().m_rank;
    uint8_t const popularity = results[i].GetInfo().m_popularity;
    uint8_t const exactMatch = results[i].GetInfo().m_exactMatch ? 1 : 0;
    // We prefer result which passed the filter even if it has lower rank / popularity / exactMatch.
    //uint8_t const filterPassed = results[i].GetInfo().m_refusedByFilter ? 0 : 2;
    // We prefer result from prevEmit over result with better filterPassed because otherwise we have
    // lots of blinking results.
    uint8_t const prevCount = prevEmit.count(results[i].GetId()) == 0 ? 0 : 3;
    uint8_t const priority = max({rank, prevCount, popularity, exactMatch}) + 2 /*filterPassed*/;
    sweeper.Add(p.x, p.y, i, priority);
  }

  vector<PreRankerResult> filtered;
  filtered.reserve(results.size());
  sweeper.Sweep([&filtered, &results](size_t i)
  {
    filtered.push_back(move(results[i]));
  });

  results.swap(filtered);
}
}  // namespace

PreRanker::PreRanker(DataSource const & dataSource, Ranker & ranker)
  : m_dataSource(dataSource), m_ranker(ranker), m_pivotFeatures(dataSource)
{
}

void PreRanker::Init(Params const & params)
{
  m_numSentResults = 0;
  m_haveFullyMatchedResult = false;
  m_results.clear();
  m_relaxedResults.clear();
  m_params = params;
  m_currEmit.clear();
}

void PreRanker::Finish(bool cancelled)
{
  m_ranker.Finish(cancelled);
}

void PreRanker::FillMissingFieldsInPreResults()
{
  MwmSet::MwmId mwmId;
  MwmSet::MwmHandle mwmHandle;
  unique_ptr<RankTable> ranks = make_unique<DummyRankTable>();
  unique_ptr<RankTable> popularityRanks = make_unique<DummyRankTable>();
  unique_ptr<LazyCentersTable> centers;
  bool pivotFeaturesInitialized = false;

  ForEachMwmOrder(m_results, [&](PreRankerResult & r)
  {
    FeatureID const & id = r.GetId();
    if (id.m_mwmId != mwmId)
    {
      mwmId = id.m_mwmId;
      mwmHandle = m_dataSource.GetMwmHandleById(mwmId);

      ranks.reset();
      centers.reset();
      if (mwmHandle.IsAlive())
      {
        auto const * value = mwmHandle.GetValue();

        ranks = RankTable::Load(value->m_cont, SEARCH_RANKS_FILE_TAG);
        popularityRanks = RankTable::Load(value->m_cont, POPULARITY_RANKS_FILE_TAG);
        centers = make_unique<LazyCentersTable>(*value);
      }
      if (!ranks)
        ranks = make_unique<DummyRankTable>();
      if (!popularityRanks)
        popularityRanks = make_unique<DummyRankTable>();
    }

    r.SetRank(ranks->Get(id.m_index));
    r.SetPopularity(popularityRanks->Get(id.m_index));

    m2::PointD center;
    if (centers && centers->Get(id.m_index, center))
    {
      r.SetDistanceToPivot(mercator::DistanceOnEarth(m_params.m_accuratePivotCenter, center));
      r.SetCenter(center);
    }
    else
    {
      auto const & editor = osm::Editor::Instance();
      if (editor.GetFeatureStatus(id.m_mwmId, id.m_index) == FeatureStatus::Created)
      {
        auto const emo = editor.GetEditedFeature(id);
        CHECK(emo, ());
        center = emo->GetMercator();
        r.SetDistanceToPivot(mercator::DistanceOnEarth(m_params.m_accuratePivotCenter, center));
        r.SetCenter(center);
      }
      else
      {
        // Possible when search while MWM is reloading or updating (!IsAlive).
        if (!pivotFeaturesInitialized)
        {
          m_pivotFeatures.SetPosition(m_params.m_accuratePivotCenter, m_params.m_scale);
          pivotFeaturesInitialized = true;
        }
        r.SetDistanceToPivot(m_pivotFeatures.GetDistanceToFeatureMeters(id));
      }
    }
  });
}

namespace
{
template <class CompT, class ContT> class CompareIndices
{
  CompT m_cmp;
  ContT const & m_cont;

public:
  CompareIndices(CompT const & cmp, ContT const & cont) : m_cmp(cmp), m_cont(cont) {}
  bool operator()(size_t l, size_t r) const
  {
    return m_cmp(m_cont[l], m_cont[r]);
  }
};
} // namespace

void PreRanker::Filter(bool viewportSearch)
{
  auto const lessForUnique = [](PreRankerResult const & lhs, PreRankerResult const & rhs)
  {
    if (lhs.GetId() != rhs.GetId())
      return lhs.GetId() < rhs.GetId();

    // It's enough to compare by tokens match and select a better one,
    // because same result can be matched by different set of tokens.
    return PreRankerResult::CompareByTokensMatch(lhs, rhs) == -1;
  };

  base::SortUnique(m_results, lessForUnique, base::EqualsBy(&PreRankerResult::GetId));

  if (viewportSearch)
    FilterForViewportSearch();

  if (m_results.size() <= BatchSize())
    return;

  vector<size_t> indices(m_results.size());
  generate(indices.begin(), indices.end(), [n = 0] () mutable { return n++; });
  unordered_set<size_t> filtered;

  auto const iBeg = indices.begin();
  auto const iMiddle = iBeg + BatchSize();
  auto const iEnd = indices.end();

  nth_element(iBeg, iMiddle, iEnd, CompareIndices(&PreRankerResult::LessDistance, m_results));
  filtered.insert(iBeg, iMiddle);

  if (!m_params.m_categorialRequest)
  {
    nth_element(iBeg, iMiddle, iEnd, CompareIndices(&PreRankerResult::LessRankAndPopularity, m_results));
    filtered.insert(iBeg, iMiddle);
    nth_element(iBeg, iMiddle, iEnd, CompareIndices(&PreRankerResult::LessByExactMatch, m_results));
    filtered.insert(iBeg, iMiddle);
  }
  else
  {
    double constexpr kPedestrianRadiusMeters = 2500.0;
    PreRankerResult::CategoriesComparator comparator;
    comparator.m_positionIsInsideViewport =
        m_params.m_position && m_params.m_viewport.IsPointInside(*m_params.m_position);
    comparator.m_detailedScale = mercator::DistanceOnEarth(m_params.m_viewport.LeftTop(),
                                                           m_params.m_viewport.RightBottom()) <
                                 2 * kPedestrianRadiusMeters;
    comparator.m_viewport = m_params.m_viewport;

    nth_element(iBeg, iMiddle, iEnd, CompareIndices(comparator, m_results));
    filtered.insert(iBeg, iMiddle);
  }

  PreResultsContainerT newResults;
  newResults.reserve(filtered.size());
  for (size_t idx : filtered)
    newResults.push_back(m_results[idx]);
  m_results.swap(newResults);
}

void PreRanker::UpdateResults(bool lastUpdate)
{
  FilterRelaxedResults(lastUpdate);
  FillMissingFieldsInPreResults();
  Filter(m_params.m_viewportSearch);
  m_numSentResults += m_results.size();
  m_ranker.AddPreRankerResults(move(m_results));
  m_results.clear();
  m_ranker.UpdateResults(lastUpdate);

  if (lastUpdate && !m_currEmit.empty())
    m_currEmit.swap(m_prevEmit);
}

void PreRanker::ClearCaches()
{
  m_pivotFeatures.Clear();
  m_prevEmit.clear();
  m_currEmit.clear();
}

void PreRanker::FilterForViewportSearch()
{
  base::EraseIf(m_results, [this](PreRankerResult const & result)
  {
    auto const & info = result.GetInfo();

    ASSERT(info.m_centerLoaded, (result.GetId()));
    if (!m_params.m_viewport.IsPointInside(info.m_center))
      return true;

    // Better criteria than previous (at first glance).
    /// @todo Probably, should show say 20-30 first results with honest ranking, but need to refactor a lot ..
    return result.SkipForViewportSearch(m_params.m_numQueryTokens);
  });

  // By VNG: Comment next statements to discard viewport filtering (displacement) for Debug purpose.
  SweepNearbyResults(m_params.m_minDistanceOnMapBetweenResults, m_prevEmit, m_results);

  for (auto const & result : m_results)
    m_currEmit.insert(result.GetId());
}

void PreRanker::FilterRelaxedResults(bool lastUpdate)
{
  auto const iEnd = m_results.end();
  if (lastUpdate)
  {
    LOG(LDEBUG, ("Flush relaxed results number:", m_relaxedResults.size()));
    m_results.insert(iEnd, make_move_iterator(m_relaxedResults.begin()), make_move_iterator(m_relaxedResults.end()));
    m_relaxedResults.clear();
  }
  else
  {
    auto const it = partition(m_results.begin(), iEnd, [](PreRankerResult const & res)
    {
      return res.IsNotRelaxed();
    });

    m_relaxedResults.insert(m_relaxedResults.end(), make_move_iterator(it), make_move_iterator(iEnd));
    m_results.erase(it, iEnd);
  }
}
}  // namespace search
