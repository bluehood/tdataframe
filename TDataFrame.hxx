// @(#)root/thread:$Id$
// Author: Enrico Guiraud, CERN  12/2016

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_TDATAFRAME
#define ROOT_TDATAFRAME

#include "TBranchElement.h"
#include "TDirectory.h"
#include "TH1F.h" // For Histo actions
#include "TROOT.h" // IsImplicitMTEnabled, GetImplicitMTPoolSize
#include "ROOT/TSpinMutex.hxx"
#include "ROOT/TTreeProcessorMT.hxx"
#include "TTreeReader.h"
#include "TTreeReaderValue.h"

#include <algorithm> // std::find
#include <array>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits> // std::decay
#include <typeinfo>
#include <vector>

// Meta programming utilities, perhaps to be moved in core/foundation
namespace ROOT {
namespace Internal {
namespace TDFTraitsUtils {
template <typename... Types>
struct TTypeList {
   static constexpr std::size_t fgSize = sizeof...(Types);
};

// extract parameter types from a callable object
template <typename T>
struct TFunctionTraits {
   using ArgTypes_t = typename TFunctionTraits<decltype(&T::operator())>::ArgTypes_t;
   using ArgTypesNoDecay_t = typename TFunctionTraits<decltype(&T::operator())>::ArgTypesNoDecay_t;
   using RetType_t = typename TFunctionTraits<decltype(&T::operator())>::RetType_t;
};

// lambdas and std::function
template <typename R, typename T, typename... Args>
struct TFunctionTraits<R (T::*)(Args...) const> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// mutable lambdas and functor classes
template <typename R, typename T, typename... Args>
struct TFunctionTraits<R (T::*)(Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// function pointers
template <typename R, typename... Args>
struct TFunctionTraits<R (*)(Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// free functions
template <typename R, typename... Args>
struct TFunctionTraits<R (Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// remove first type from TypeList
template <typename>
struct TRemoveFirst { };

template <typename T, typename... Args>
struct TRemoveFirst<TTypeList<T, Args...>> {
   using Types_t = TTypeList<Args...>;
};

// return wrapper around f that prepends an `unsigned int slot` parameter
template <typename R, typename F, typename... Args>
std::function<R(unsigned int, Args...)> AddSlotParameter(F f, TTypeList<Args...>)
{
   return [f](unsigned int, Args... a) -> R { return f(a...); };
}

// compile-time integer sequence generator
// e.g. calling TGenStaticSeq<3>::type() instantiates a TStaticSeq<0,1,2>
template <int...>
struct TStaticSeq { };

template <int N, int... S>
struct TGenStaticSeq : TGenStaticSeq<N - 1, N - 1, S...> { };

template <int... S>
struct TGenStaticSeq<0, S...> {
   using Type_t = TStaticSeq<S...>;
};

template <typename T>
struct TIsContainer {
   using Test_t = typename std::decay<T>::type;

   template <typename A>
   static constexpr bool Test(A *pt, A const *cpt = nullptr, decltype(pt->begin()) * = nullptr,
                              decltype(pt->end()) * = nullptr, decltype(cpt->begin()) * = nullptr,
                              decltype(cpt->end()) * = nullptr, typename A::iterator *pi = nullptr,
                              typename A::const_iterator *pci = nullptr, typename A::value_type *pv = nullptr)
   {
      using It_t = typename A::iterator;
      using CIt_t = typename A::const_iterator;
      using V_t = typename A::value_type;
      return std::is_same<Test_t, std::vector<bool>>::value ||
             (std::is_same<decltype(pt->begin()), It_t>::value &&
              std::is_same<decltype(pt->end()), It_t>::value &&
              std::is_same<decltype(cpt->begin()), CIt_t>::value &&
              std::is_same<decltype(cpt->end()), CIt_t>::value &&
              std::is_same<decltype(**pi), V_t &>::value &&
              std::is_same<decltype(**pci), V_t const &>::value);
   }

   template <typename A>
   static constexpr bool Test(...)
   {
      return false;
   }

   static const bool fgValue = Test<Test_t>(nullptr);
};

} // end NS TDFTraitsUtils

} // end NS Internal

} // end NS ROOT

namespace ROOT {

using BranchNames = std::vector<std::string>;

// Fwd declarations
namespace Details {
class TDataFrameImpl;
}

/// Smart pointer for the return type of actions
/**
* \class ROOT::TActionResultProxy
* \brief A wrapper around the result of TDataFrame actions able to trigger calculations lazily.
* \tparam T Type of the action result
*
* A smart pointer which allows to access the result of a TDataFrame action. The
* methods of the encapsulated object can be accessed via the arrow operator.
* Upon invocation of the arrow operator or dereferencing (`operator*`), the
* loop on the events and calculations of all scheduled actions are executed
* if needed.
*/
template <typename T>
class TActionResultProxy {

   template<typename V, bool isCont = Internal::TDFTraitsUtils::TIsContainer<V>::fgValue>
   struct TIterationHelper{
      using Iterator_t = int;
      void GetBegin(const V& ){static_assert(sizeof(V) == 0, "It does not make sense to ask begin for this class.");}
      void GetEnd(const V& ){static_assert(sizeof(V) == 0, "It does not make sense to ask end for this class.");}
   };

   template<typename V>
   struct TIterationHelper<V,true>{
      using Iterator_t = decltype(std::begin(std::declval<V>()));
      static Iterator_t GetBegin(const V& v) {return std::begin(v);};
      static Iterator_t GetEnd(const V& v) {return std::end(v);};
   };

   using SPT_t = std::shared_ptr<T> ;
   using SPTDFI_t = std::shared_ptr<Details::TDataFrameImpl>;
   using WPTDFI_t = std::weak_ptr<Details::TDataFrameImpl>;
   using ShrdPtrBool_t = std::shared_ptr<bool>;
   friend class Details::TDataFrameImpl;

   ShrdPtrBool_t fReadiness = std::make_shared<bool>(false); ///< State registered also in the TDataFrameImpl until the event loop is executed
   WPTDFI_t fFirstData;                                      ///< Original TDataFrame
   SPT_t fObjPtr;                                            ///< Shared pointer encapsulating the wrapped result
   /// Triggers the event loop in the TDataFrameImpl instance to which it's associated via the fFirstData
   void TriggerRun();
   /// Get the pointer to the encapsulated result.
   /// Ownership is not transferred to the caller.
   /// Triggers event loop and execution of all actions booked in the associated TDataFrameImpl.
   T *Get()
   {
      if (!*fReadiness) TriggerRun();
      return fObjPtr.get();
   }
   TActionResultProxy(SPT_t objPtr, ShrdPtrBool_t readiness, SPTDFI_t firstData)
      : fReadiness(readiness), fFirstData(firstData), fObjPtr(objPtr) { }
   /// Factory to allow to keep the constructor private
   static TActionResultProxy<T> MakeActionResultPtr(SPT_t objPtr, ShrdPtrBool_t readiness, SPTDFI_t firstData)
   {
      return TActionResultProxy(objPtr, readiness, firstData);
   }
public:
   TActionResultProxy() = delete;
   /// Get a reference to the encapsulated object.
   /// Triggers event loop and execution of all actions booked in the associated TDataFrameImpl.
   T &operator*() { return *Get(); }
   /// Get a pointer to the encapsulated object.
   /// Ownership is not transferred to the caller.
   /// Triggers event loop and execution of all actions booked in the associated TDataFrameImpl.
   T *operator->() { return Get(); }
   /// Return an iterator to the beginning of the contained object if this makes
   /// sense, throw a compilation error otherwise
   typename TIterationHelper<T>::Iterator_t begin()
   {
      if (!*fReadiness) TriggerRun();
      return TIterationHelper<T>::GetBegin(*fObjPtr);
   }
   /// Return an iterator to the end of the contained object if this makes
   /// sense, throw a compilation error otherwise
   typename TIterationHelper<T>::Iterator_t end()
   {
      if (!*fReadiness) TriggerRun();
      return TIterationHelper<T>::GetEnd(*fObjPtr);
   }
};

} // end NS ROOT

// Internal classes
namespace ROOT {

namespace Details {
class TDataFrameImpl;
}

namespace Internal {

unsigned int GetNSlots() {
   unsigned int nSlots = 1;
#ifdef R__USE_IMT
   if (ROOT::IsImplicitMTEnabled()) nSlots = ROOT::GetImplicitMTPoolSize();
#endif // R__USE_IMT
   return nSlots;
}

using TVBPtr_t = std::shared_ptr<TTreeReaderValueBase>;
using TVBVec_t = std::vector<TVBPtr_t>;

template <int... S, typename... BranchTypes>
TVBVec_t BuildReaderValues(TTreeReader &r, const BranchNames &bl, const BranchNames &tmpbl,
                           TDFTraitsUtils::TTypeList<BranchTypes...>,
                           TDFTraitsUtils::TStaticSeq<S...>)
{
   // isTmpBranch has length bl.size(). Elements are true if the corresponding
   // branch is a "fake" branch created with AddBranch, false if they are
   // actual branches present in the TTree.
   std::array<bool, sizeof...(S)> isTmpBranch;
   for (unsigned int i = 0; i < isTmpBranch.size(); ++i)
      isTmpBranch[i] = std::find(tmpbl.begin(), tmpbl.end(), bl.at(i)) != tmpbl.end();

   // Build vector of pointers to TTreeReaderValueBase.
   // tvb[i] points to a TTreeReaderValue specialized for the i-th BranchType,
   // corresponding to the i-th branch in bl
   // For temporary branches (declared with AddBranch) a nullptr is created instead
   // S is expected to be a sequence of sizeof...(BranchTypes) integers
   TVBVec_t tvb{isTmpBranch[S] ? nullptr : std::make_shared<TTreeReaderValue<BranchTypes>>(
                                            r, bl.at(S).c_str())...}; // "..." expands BranchTypes and S simultaneously

   return tvb;
}

template <typename Filter>
void CheckFilter(Filter f)
{
   using FilterRet_t = typename TDFTraitsUtils::TFunctionTraits<Filter>::RetType_t;
   static_assert(std::is_same<FilterRet_t, bool>::value, "filter functions must return a bool");
}

void CheckTmpBranch(const std::string& branchName, TTree *treePtr)
{
   auto branch = treePtr->GetBranch(branchName.c_str());
   if (branch != nullptr) {
      auto msg = "branch \"" + branchName + "\" already present in TTree";
      throw std::runtime_error(msg);
   }
}

/// Returns local BranchNames or default BranchNames according to which one should be used
const BranchNames &PickBranchNames(unsigned int nArgs, const BranchNames &bl, const BranchNames &defBl)
{
   bool useDefBl = false;
   if (nArgs != bl.size()) {
      if (bl.size() == 0 && nArgs == defBl.size()) {
         useDefBl = true;
      } else {
         auto msg = "mismatch between number of filter arguments (" + std::to_string(nArgs) +
                    ") and number of branches (" + std::to_string(bl.size() ? bl.size() : defBl.size()) + ")";
         throw std::runtime_error(msg);
      }
   }

   return useDefBl ? defBl : bl;
}

class TDataFrameActionBase {
public:
   virtual ~TDataFrameActionBase() {}
   virtual void Run(unsigned int slot, int entry) = 0;
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
};

using ActionBasePtr_t = std::shared_ptr<TDataFrameActionBase>;
using ActionBaseVec_t = std::vector<ActionBasePtr_t>;

// Forward declarations
template <int S, typename T>
T &GetBranchValue(TVBPtr_t &readerValues, unsigned int slot, int entry, const std::string &branch,
                  std::weak_ptr<Details::TDataFrameImpl> df);

template <typename F, typename PrevDataFrame>
class TDataFrameAction final : public TDataFrameActionBase {
   using BranchTypes_t = typename TDFTraitsUtils::TRemoveFirst<typename TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t>::Types_t;
   using TypeInd_t = typename TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;

   F fAction;
   const BranchNames fBranches;
   const BranchNames fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<Details::TDataFrameImpl> fFirstData;
   std::vector<TVBVec_t> fReaderValues;

public:
   TDataFrameAction(F f, const BranchNames &bl, std::weak_ptr<PrevDataFrame> pd)
      : fAction(f), fBranches(bl), fTmpBranches(pd.lock()->GetTmpBranches()), fPrevData(pd.lock().get()),
        fFirstData(pd.lock()->GetDataFrame()) { }

   TDataFrameAction(const TDataFrameAction &) = delete;

   void Run(unsigned int slot, int entry)
   {
      // check if entry passes all filters
      if (CheckFilters(slot, entry)) ExecuteAction(slot, entry);
   }

   bool CheckFilters(unsigned int slot, int entry)
   {
      // start the recursive chain of CheckFilters calls
      return fPrevData->CheckFilters(slot, entry);
   }

   void ExecuteAction(unsigned int slot, int entry) { ExecuteActionHelper(slot, entry, TypeInd_t(), BranchTypes_t()); }

   void CreateSlots(unsigned int nSlots) { fReaderValues.resize(nSlots); }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = ROOT::Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   template <int... S, typename... BranchTypes>
   void ExecuteActionHelper(unsigned int slot, int entry,
                            TDFTraitsUtils::TStaticSeq<S...>,
                            TDFTraitsUtils::TTypeList<BranchTypes...>)
   {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      fAction(slot, GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
   }
};

namespace Operations {
using namespace Internal::TDFTraitsUtils;
using Count_t = unsigned long;

class CountOperation {
   unsigned int *fResultCount;
   std::vector<Count_t> fCounts;

public:
   CountOperation(unsigned int *resultCount, unsigned int nSlots) : fResultCount(resultCount), fCounts(nSlots, 0) {}

   void Exec(unsigned int slot)
   {
      fCounts[slot]++;
   }

   ~CountOperation()
   {
      *fResultCount = 0;
      for (auto &c : fCounts) {
         *fResultCount += c;
      }
   }
};

class FillOperation {
   // this sets a total initial size of 16 MB for the buffers (can increase)
   static constexpr unsigned int fgTotalBufSize = 2097152;
   using BufEl_t = double;
   using Buf_t = std::vector<BufEl_t>;

   std::vector<Buf_t> fBuffers;
   std::shared_ptr<TH1F> fResultHist;
   unsigned int fBufSize;
   Buf_t fMin;
   Buf_t fMax;

   template <typename T>
   void UpdateMinMax(unsigned int slot, T v) {
      auto& thisMin = fMin[slot];
      auto& thisMax = fMax[slot];
      thisMin = std::min(thisMin, (BufEl_t)v);
      thisMax = std::max(thisMax, (BufEl_t)v);
   }

public:
   FillOperation(std::shared_ptr<TH1F> h, unsigned int nSlots) : fResultHist(h),
                                                                 fBufSize (fgTotalBufSize / nSlots),
                                                                 fMin(nSlots, std::numeric_limits<BufEl_t>::max()),
                                                                 fMax(nSlots, std::numeric_limits<BufEl_t>::min())
   {
      fBuffers.reserve(nSlots);
      for (unsigned int i=0; i<nSlots; ++i) {
         Buf_t v;
         v.reserve(fBufSize);
         fBuffers.emplace_back(v);
      }
   }

   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      UpdateMinMax(slot, v);
      fBuffers[slot].emplace_back(v);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      auto& thisBuf = fBuffers[slot];
      for (auto& v : vs) {
         UpdateMinMax(slot, v);
         thisBuf.emplace_back(v); // TODO: Can be optimised in case T == BufEl_t
      }
   }

   ~FillOperation()
   {

      BufEl_t globalMin = *std::min_element(fMin.begin(), fMin.end());
      BufEl_t globalMax = *std::max_element(fMax.begin(), fMax.end());

      if (fResultHist->CanExtendAllAxes() &&
          globalMin != std::numeric_limits<BufEl_t>::max() &&
          globalMax != std::numeric_limits<BufEl_t>::min()) {
         auto xaxis = fResultHist->GetXaxis();
         fResultHist->ExtendAxis(globalMin, xaxis);
         fResultHist->ExtendAxis(globalMax, xaxis);
      }

      for (auto& buf : fBuffers) {
         Buf_t w(buf.size(),1); // A bug in FillN?
         fResultHist->FillN(buf.size(), buf.data(),  w.data());
      }
   }
};


class FillTOOperation {
   TThreadedObject<TH1F> fTo;

public:

   FillTOOperation(std::shared_ptr<TH1F> h, unsigned int nSlots) : fTo(*h)
   {
      fTo.SetAtSlot(0, h);
      // Initialise all other slots
      for (unsigned int i = 0 ; i < nSlots; ++i) {
         fTo.GetAtSlot(i);
      }
   }

   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fTo.GetAtSlotUnchecked(slot)->Fill(v);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      auto thisSlotH = fTo.GetAtSlotUnchecked(slot);
      for (auto& v : vs) {
         thisSlotH->Fill(v); // TODO: Can be optimised in case T == vector<double>
      }
   }

   ~FillTOOperation()
   {
      fTo.Merge();
   }

};

// note: changes to this class should probably be replicated in its partial
// specialization below
template<typename T, typename COLL>
class TakeOperation {
   std::vector<std::shared_ptr<COLL>> fColls;
public:
   TakeOperation(std::shared_ptr<COLL> resultColl, unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i)
         fColls.emplace_back(std::make_shared<COLL>());
   }

   template <typename V, typename std::enable_if<!TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(V v, unsigned int slot)
   {
      fColls[slot]->emplace_back(v);
   }

   template <typename V, typename std::enable_if<TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(const V &vs, unsigned int slot)
   {
      auto thisColl = fColls[slot];
      thisColl.insert(std::begin(thisColl), std::begin(vs), std::begin(vs));
   }

   ~TakeOperation()
   {
      auto rColl = fColls[0];
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto& coll = fColls[i];
         for (T &v : *coll) {
            rColl->emplace_back(v);
         }
      }
   }
};

// note: changes to this class should probably be replicated in its unspecialized
// declaration above
template<typename T>
class TakeOperation<T, std::vector<T>> {
   std::vector<std::shared_ptr<std::vector<T>>> fColls;
public:
   TakeOperation(std::shared_ptr<std::vector<T>> resultColl, unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i) {
         auto v = std::make_shared<std::vector<T>>();
         v->reserve(1024);
         fColls.emplace_back(v);
      }
   }

   template <typename V, typename std::enable_if<!TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(V v, unsigned int slot)
   {
      fColls[slot]->emplace_back(v);
   }

   template <typename V, typename std::enable_if<TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(const V &vs, unsigned int slot)
   {
      auto thisColl = fColls[slot];
      thisColl->insert(std::begin(thisColl), std::begin(vs), std::begin(vs));
   }

   ~TakeOperation()
   {
      unsigned int totSize = 0;
      for (auto& coll : fColls) totSize += coll->size();
      auto rColl = fColls[0];
      rColl->reserve(totSize);
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto& coll = fColls[i];
         rColl->insert(rColl->end(), coll->begin(), coll->end());
      }
   }
};

class MinOperation {
   double *fResultMin;
   std::vector<double> fMins;

public:
   MinOperation(double *minVPtr, unsigned int nSlots)
      : fResultMin(minVPtr), fMins(nSlots, std::numeric_limits<double>::max()) { }
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fMins[slot] = std::min((double)v, fMins[slot]);
   }
   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) fMins[slot] = std::min((double)v, fMins[slot]);
   }
   ~MinOperation()
   {
      *fResultMin = std::numeric_limits<double>::max();
      for (auto &m : fMins) *fResultMin = std::min(m, *fResultMin);
   }
};

class MaxOperation {
   double *fResultMax;
   std::vector<double> fMaxs;

public:
   MaxOperation(double *maxVPtr, unsigned int nSlots)
      : fResultMax(maxVPtr), fMaxs(nSlots, std::numeric_limits<double>::min()) { }
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fMaxs[slot] = std::max((double)v, fMaxs[slot]);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) fMaxs[slot] = std::max((double)v, fMaxs[slot]);
   }

   ~MaxOperation()
   {
      *fResultMax = std::numeric_limits<double>::min();
      for (auto &m : fMaxs) {
         *fResultMax = std::max(m, *fResultMax);
      }
   }
};

class MeanOperation {
   double *fResultMean;
   std::vector<Count_t> fCounts;
   std::vector<double> fSums;

public:
   MeanOperation(double *meanVPtr, unsigned int nSlots) : fResultMean(meanVPtr), fCounts(nSlots, 0), fSums(nSlots, 0) {}
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fSums[slot] += v;
      fCounts[slot] ++;
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) {
         fSums[slot] += v;
         fCounts[slot]++;
      }
   }

   ~MeanOperation()
   {
      double sumOfSums = 0;
      for (auto &s : fSums) sumOfSums += s;
      Count_t sumOfCounts = 0;
      for (auto &c : fCounts) sumOfCounts += c;
      *fResultMean = sumOfSums / (sumOfCounts > 0 ? sumOfCounts : 1);
   }
};

} // end of NS Operations

enum class EActionType : short { kHisto1D, kMin, kMax, kMean };

} // end NS Internal

namespace Details {
// forward declarations for TDataFrameInterface
template <typename F, typename PrevData>
class TDataFrameFilter;
template <typename F, typename PrevData>
class TDataFrameBranch;
class TDataFrameImpl;
}

/**
* \class ROOT::TDataFrameInterface
* \brief The public interface to the TDataFrame federation of classes: TDataFrameImpl, TDataFrameFilter, TDataFrameBranch
* \tparam T One of the TDataFrameImpl, TDataFrameFilter, TDataFrameBranch classes. The user never specifies this type manually.
*/
template <typename Proxied>
class TDataFrameInterface {
   template<typename T> friend class TDataFrameInterface;
public:
   ////////////////////////////////////////////////////////////////////////////
   /// \brief Build the dataframe
   /// \param[in] treeName Name of the tree contained in the directory
   /// \param[in] dirPtr TDirectory where the tree is stored, e.g. a TFile.
   /// \param[in] defaultBranches Collection of default branches.
   ///
   /// The default branches are looked at in case no branch is specified in the
   /// booking of actions or transformations.
   TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr, const BranchNames &defaultBranches = {});

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Build the dataframe
   /// \param[in] tree The tree or chain to be studied.
   /// \param[in] defaultBranches Collection of default branches.
   ///
   /// The default branches are looked at in case no branch is specified in the
   /// booking of actions or transformations.
   TDataFrameInterface(TTree &tree, const BranchNames &defaultBranches = {});

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Append a filter to the call graph.
   /// \param[in] f Function, lambda expression, functor class or any other callable object. It must return a `bool` signalling whether the event has passed the selection (true) or not (false).
   /// \param[in] bl Names of the branches in input to the filter function.
   ///
   /// Append a filter node at the point of the call graph corresponding to the
   /// object this method is called on.
   /// The callable `f` should not have side-effects (e.g. modification of an
   /// external or static variable) to ensure correct results when implicit
   /// multi-threading is active.
   ///
   /// TDataFrame only evaluates filters when necessary: if multiple filters
   /// are chained one after another, they are executed in order and the first
   /// one returning false causes the event to be discarded.
   /// Even if multiple actions or transformations depend on the same filter,
   /// it is executed once per entry. If its result is requested more than
   /// once, the cached result is served.
   template <typename F>
   TDataFrameInterface<Details::TDataFrameFilter<F, Proxied>> Filter(F f, const BranchNames &bl = {})
   {
      ROOT::Internal::CheckFilter(f);
      auto df = GetDataFrameChecked();
      const BranchNames &defBl = df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchNames &actualBl = Internal::PickBranchNames(nArgs, bl, defBl);
      using DFF_t = Details::TDataFrameFilter<F, Proxied>;
      auto FilterPtr = std::make_shared<DFF_t> (f, actualBl, fProxiedPtr);
      TDataFrameInterface<DFF_t> tdf_f(FilterPtr);
      df->Book(FilterPtr);
      return tdf_f;
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Creates a temporary branch
   /// \param[in] name The name of the temporary branch.
   /// \param[in] expression Function, lambda expression, functor class or any other callable object producing the temporary value. Returns the value that will be assigned to the temporary branch.
   /// \param[in] bl Names of the branches in input to the producer function.
   ///
   /// Create a temporary branch that will be visible from all subsequent nodes
   /// of the functional chain. The `expression` is only evaluated for entries that pass
   /// all the preceding filters.
   /// A new variable is created called `name`, accessible as if it was contained
   /// in the dataset from subsequent transformations/actions.
   ///
   /// Use cases include:
   ///
   /// * caching the results of complex calculations for easy and efficient multiple access
   /// * extraction of quantities of interest from complex objects
   /// * branch aliasing, i.e. changing the name of a branch
   ///
   /// An exception is thrown if the name of the new branch is already in use
   /// for another branch in the TTree.
   template <typename F>
   TDataFrameInterface<Details::TDataFrameBranch<F, Proxied>>
   AddBranch(const std::string &name, F expression, const BranchNames &bl = {})
   {
      auto df = GetDataFrameChecked();
      ROOT::Internal::CheckTmpBranch(name, df->GetTree());
      const BranchNames &defBl = df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchNames &actualBl = Internal::PickBranchNames(nArgs, bl, defBl);
      using DFB_t = Details::TDataFrameBranch<F, Proxied>;
      auto BranchPtr = std::make_shared<DFB_t>(name, expression, actualBl, fProxiedPtr);
      TDataFrameInterface<DFB_t> tdf_b(BranchPtr);
      df->Book(BranchPtr);
      return tdf_b;
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Execute a user-defined function on each entry (*instant action*)
   /// \param[in] f Function, lambda expression, functor class or any other callable object performing user defined calculations.
   /// \param[in] bl Names of the branches in input to the user function.
   ///
   /// The callable `f` is invoked once per entry. This is an *instant action*:
   /// upon invocation, an event loop as well as execution of all scheduled actions
   /// is triggered.
   /// Users are responsible for the thread-safety of this callable when executing
   /// with implicit multi-threading enabled (i.e. ROOT::EnableImplicitMT).
   template <typename F>
   void Foreach(F f, const BranchNames &bl = {})
   {
      namespace IU = Internal::TDFTraitsUtils;
      using ArgTypes_t = typename IU::TFunctionTraits<decltype(f)>::ArgTypesNoDecay_t;
      using RetType_t = typename IU::TFunctionTraits<decltype(f)>::RetType_t;
      auto fWithSlot = IU::AddSlotParameter<RetType_t>(f, ArgTypes_t());
      ForeachSlot(fWithSlot, bl);
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Execute a user-defined function requiring a processing slot index on each entry (*instant action*)
   /// \param[in] f Function, lambda expression, functor class or any other callable object performing user defined calculations.
   /// \param[in] bl Names of the branches in input to the user function.
   ///
   /// Same as `Foreach`, but the user-defined function takes an extra
   /// `unsigned int` as its first parameter, the *processing slot index*.
   /// This *slot index* will be assigned a different value, `0` to `poolSize - 1`,
   /// for each thread of execution.
   /// This is meant as a helper in writing thread-safe `Foreach`
   /// actions when using `TDataFrame` after `ROOT::EnableImplicitMT()`.
   /// The user-defined processing callable is able to follow different
   /// *streams of processing* indexed by the first parameter.
   /// `ForeachSlot` works just as well with single-thread execution: in that
   /// case `slot` will always be `0`.
   template<typename F>
   void ForeachSlot(F f, const BranchNames &bl = {}) {
      auto df = GetDataFrameChecked();
      const BranchNames &defBl= df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchNames &actualBl = Internal::PickBranchNames(nArgs-1, bl, defBl);
      using DFA_t  = Internal::TDataFrameAction<decltype(f), Proxied>;
      df->Book(std::make_shared<DFA_t>(f, actualBl, fProxiedPtr));
      df->Run();
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Return the number of entries processed (*lazy action*)
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   TActionResultProxy<unsigned int> Count()
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df->GetNSlots();
      auto cShared = std::make_shared<unsigned int>(0);
      auto c = df->MakeActionResultPtr(cShared);
      auto cPtr = cShared.get();
      auto cOp = std::make_shared<Internal::Operations::CountOperation>(cPtr, nSlots);
      auto countAction = [cOp](unsigned int slot) mutable { cOp->Exec(slot); };
      BranchNames bl = {};
      using DFA_t = Internal::TDataFrameAction<decltype(countAction), Proxied>;
      df->Book(std::shared_ptr<DFA_t>(new DFA_t(countAction, bl, fProxiedPtr)));
      return c;
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Return a collection of values of a branch (*lazy action*)
   /// \tparam T The type of the branch.
   /// \tparam COLL The type of collection used to store the values.
   /// \param[in] branchName The name of the branch of which the values are to be collected
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T, typename COLL = std::vector<T>>
   TActionResultProxy<COLL> Take(const std::string &branchName = "")
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df->GetNSlots();
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "get the values of the branch");
      auto valuesPtr = std::make_shared<COLL>();
      auto values = df->MakeActionResultPtr(valuesPtr);
      auto getOp = std::make_shared<Internal::Operations::TakeOperation<T,COLL>>(valuesPtr, nSlots);
      auto getAction = [getOp] (unsigned int slot , const T &v) mutable { getOp->Exec(v, slot); };
      BranchNames bl = {theBranchName};
      using DFA_t = Internal::TDataFrameAction<decltype(getAction), Proxied>;
      df->Book(std::shared_ptr<DFA_t>(new DFA_t(getAction, bl, fProxiedPtr)));
      return values;
   }


   ////////////////////////////////////////////////////////////////////////////
   /// \brief Fill and return a one-dimensional histogram with the values of a branch (*lazy action*)
   /// \tparam T The type of the branch the values of which are used to fill the histogram.
   /// \param[in] branchName The name of the branch of which the values are to be collected.
   /// \param[in] model The model to be copied to build the new return value.
   ///
   /// If no branch type is specified, the implementation will try to guess one.
   /// The returned histogram is independent of the input one.
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T = double>
   TActionResultProxy<TH1F> Histo(const std::string &branchName, const TH1F &model)
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>(model);
      return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName, h);
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Fill and return a one-dimensional histogram with the values of a branch (*lazy action*)
   /// \tparam T The type of the branch the values of which are used to fill the histogram.
   /// \param[in] branchName The name of the branch of which the values are to be collected.
   /// \param[in] nbins The number of bins.
   /// \param[in] minVal The lower value of the xaxis.
   /// \param[in] maxVal The upper value of the xaxis.
   ///
   /// If no branch type is specified, the implementation will try to guess one.
   ///
   /// If no axes boundaries are specified, all entries are buffered: at the end of
   /// the loop on the entries, the histogram is filled. If the axis boundaries are
   /// specified, the histogram (or histograms in the parallel case) are filled. This
   /// latter mode may result in a reduced memory footprint.
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T = double>
   TActionResultProxy<TH1F> Histo(const std::string &branchName = "", int nBins = 128, double minVal = 0.,
                                double maxVal = 0.)
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>("", "", nBins, minVal, maxVal);
      if (minVal == maxVal) {
         h->SetCanExtend(TH1::kAllAxes);
      }
      return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName, h);
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Return the minimum of processed branch values (*lazy action*)
   /// \tparam T The type of the branch.
   /// \param[in] branchName The name of the branch to be treated.
   ///
   /// If no branch type is specified, the implementation will try to guess one.
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T = double>
   TActionResultProxy<double> Min(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the minumum");
      auto minV = std::make_shared<T>(std::numeric_limits<T>::max());
      return CreateAction<T, Internal::EActionType::kMin>(theBranchName, minV);
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Return the maximum of processed branch values (*lazy action*)
   /// \tparam T The type of the branch.
   /// \param[in] branchName The name of the branch to be treated.
   ///
   /// If no branch type is specified, the implementation will try to guess one.
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T = double>
   TActionResultProxy<double> Max(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the maximum");
      auto maxV = std::make_shared<T>(std::numeric_limits<T>::min());
      return CreateAction<T, Internal::EActionType::kMax>(theBranchName, maxV);
   }

   ////////////////////////////////////////////////////////////////////////////
   /// \brief Return the mean of processed branch values (*lazy action*)
   /// \tparam T The type of the branch.
   /// \param[in] branchName The name of the branch to be treated.
   ///
   /// If no branch type is specified, the implementation will try to guess one.
   ///
   /// This action is *lazy*: upon invocation of this method the calculation is
   /// booked but not executed. See TActionResultProxy documentation.
   template <typename T = double>
   TActionResultProxy<double> Mean(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the mean");
      auto meanV = std::make_shared<T>(0);
      return CreateAction<T, Internal::EActionType::kMean>(theBranchName, meanV);
   }

private:
   TDataFrameInterface(std::shared_ptr<Proxied> proxied) : fProxiedPtr(proxied) {}

   /// Get the TDataFrameImpl if reachable. If not, throw.
   std::shared_ptr<Details::TDataFrameImpl> GetDataFrameChecked()
   {
      auto df = fProxiedPtr->GetDataFrame().lock();
      if (!df) {
         throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
      }
      return df;
   }

   void GetDefaultBranchName(std::string &theBranchName, const std::string &actionNameForErr)
   {
      if (theBranchName.empty()) {
         // Try the default branch if possible
         auto df = GetDataFrameChecked();
         const BranchNames &defBl = df->GetDefaultBranches();
         if (defBl.size() == 1) {
            theBranchName = defBl[0];
         } else {
            std::string msg("No branch in input to ");
            msg += actionNameForErr;
            msg += " and default branch list has size ";
            msg += std::to_string(defBl.size());
            msg += ", need 1";
            throw std::runtime_error(msg);
         }
      }
   }

   template <typename BranchType, typename ActionResultType, enum Internal::EActionType, typename ThisType>
   struct SimpleAction {};

   template <typename BranchType, typename ThisType>
   struct SimpleAction<BranchType, TH1F, Internal::EActionType::kHisto1D, ThisType> {
      static TActionResultProxy<TH1F> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                 std::shared_ptr<TH1F> h, unsigned int nSlots)
      {
         // we use a shared_ptr so that the operation has the same scope of the lambda
         // and therefore of the TDataFrameAction that contains it: merging of results
         // from different threads is performed in the operation's destructor, at the
         // moment when the TDataFrameAction is deleted by TDataFrameImpl
         BranchNames bl = {theBranchName};
         auto df = thisFrame->GetDataFrameChecked();
         auto xaxis = h->GetXaxis();
         auto hasAxisLimits = !(xaxis->GetXmin() == 0. && xaxis->GetXmax() == 0.);

         if (hasAxisLimits) {
            auto fillTOOp = std::make_shared<Internal::Operations::FillTOOperation>(h, nSlots);
            auto fillLambda = [fillTOOp](unsigned int slot, const BranchType &v) mutable { fillTOOp->Exec(v, slot); };
            using DFA_t = Internal::TDataFrameAction<decltype(fillLambda), Proxied>;
            df->Book(std::make_shared<DFA_t>(fillLambda, bl, thisFrame->fProxiedPtr));
         } else {
            auto fillOp = std::make_shared<Internal::Operations::FillOperation>(h, nSlots);
            auto fillLambda = [fillOp](unsigned int slot, const BranchType &v) mutable { fillOp->Exec(v, slot); };
            using DFA_t = Internal::TDataFrameAction<decltype(fillLambda), Proxied>;
            df->Book(std::make_shared<DFA_t>(fillLambda, bl, thisFrame->fProxiedPtr));
         }
         return df->MakeActionResultPtr(h);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMin, ThisType> {
      static TActionResultProxy<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> minV, unsigned int nSlots)
      {
         // see "TActionResultProxy<TH1F> BuildAndBook" for why this is a shared_ptr
         auto minOp = std::make_shared<Internal::Operations::MinOperation>(minV.get(), nSlots);
         auto minOpLambda = [minOp](unsigned int slot, const BranchType &v) mutable { minOp->Exec(v, slot); };
         BranchNames bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(minOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(minOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(minV);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMax, ThisType> {
      static TActionResultProxy<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> maxV, unsigned int nSlots)
      {
         // see "TActionResultProxy<TH1F> BuildAndBook" for why this is a shared_ptr
         auto maxOp = std::make_shared<Internal::Operations::MaxOperation>(maxV.get(), nSlots);
         auto maxOpLambda = [maxOp](unsigned int slot, const BranchType &v) mutable { maxOp->Exec(v, slot); };
         BranchNames bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(maxOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(maxOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(maxV);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMean, ThisType> {
      static TActionResultProxy<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> meanV, unsigned int nSlots)
      {
         // see "TActionResultProxy<TH1F> BuildAndBook" for why this is a shared_ptr
         auto meanOp = std::make_shared<Internal::Operations::MeanOperation>(meanV.get(), nSlots);
         auto meanOpLambda = [meanOp](unsigned int slot, const BranchType &v) mutable { meanOp->Exec(v, slot); };
         BranchNames bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(meanOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(meanOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(meanV);
      }
   };

   template <typename BranchType, Internal::EActionType ActionType, typename ActionResultType>
   TActionResultProxy<ActionResultType> CreateAction(const std::string & theBranchName,
                                                   std::shared_ptr<ActionResultType> r)
   {
      // More types can be added at will at the cost of some compilation time and size of binaries.
      using ART_t = ActionResultType;
      using TT_t = decltype(this);
      const auto at = ActionType;
      auto df = GetDataFrameChecked();
      auto tree = static_cast<TTree*>(df->GetDirectory()->Get(df->GetTreeName().c_str()));
      auto branch = tree->GetBranch(theBranchName.c_str());
      unsigned int nSlots = df->GetNSlots();
      if (!branch) {
         // temporary branch
         const auto &type_id = df->GetBookedBranch(theBranchName).GetTypeId();
         if (type_id == typeid(char)) {
            return SimpleAction<char, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(int)) {
            return SimpleAction<int, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(double)) {
            return SimpleAction<double, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(double)) {
            return SimpleAction<double, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(std::vector<double>)) {
            return SimpleAction<std::vector<double>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(std::vector<float>)) {
            return SimpleAction<std::vector<float>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      }
      // real branch
      auto branchEl = dynamic_cast<TBranchElement *>(branch);
      if (!branchEl) { // This is a fundamental type
         auto title    = branch->GetTitle();
         auto typeCode = title[strlen(title) - 1];
         if (typeCode == 'B') {
            return SimpleAction<char, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'b') { return SimpleAction<Uchar, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'S') { return SimpleAction<Short_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 's') { return SimpleAction<UShort_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'I') {
            return SimpleAction<int, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'i') { return SimpleAction<unsigned int , ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'F') { return SimpleAction<float, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'D') {
            return SimpleAction<double, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'L') { return SimpleAction<Long64_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'l') { return SimpleAction<ULong64_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'O') {
            return SimpleAction<bool, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      } else {
         std::string typeName = branchEl->GetTypeName();
         if (typeName == "vector<double>") {
            return SimpleAction<std::vector<double>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (typeName == "vector<float>") {
            return SimpleAction<std::vector<float>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      }
      return SimpleAction<BranchType, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
   }

   std::shared_ptr<Proxied> fProxiedPtr;
};

using TDataFrame = TDataFrameInterface<ROOT::Details::TDataFrameImpl>;

namespace Details {

class TDataFrameBranchBase {
public:
   virtual ~TDataFrameBranchBase() {}
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
   virtual std::string GetName() const       = 0;
   virtual void *GetValue(unsigned int slot, int entry) = 0;
   virtual const std::type_info &GetTypeId() const = 0;
};
using TmpBranchBasePtr_t = std::shared_ptr<TDataFrameBranchBase>;

template <typename F, typename PrevData>
class TDataFrameBranch final : public TDataFrameBranchBase {
   using BranchTypes_t = typename Internal
   ::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;
   using RetType_t = typename Internal::TDFTraitsUtils::TFunctionTraits<F>::RetType_t;

   const std::string fName;
   F fExpression;
   const BranchNames fBranches;
   BranchNames fTmpBranches;
   std::vector<ROOT::Internal::TVBVec_t> fReaderValues;
   std::vector<std::shared_ptr<RetType_t>> fLastResultPtr;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   PrevData *fPrevData;
   std::vector<int> fLastCheckedEntry = {-1};

public:
   TDataFrameBranch(const std::string &name, F expression, const BranchNames &bl, std::shared_ptr<PrevData> pd)
      : fName(name), fExpression(expression), fBranches(bl), fTmpBranches(pd->GetTmpBranches()),
        fFirstData(pd->GetDataFrame()), fPrevData(pd.get())
   {
      fTmpBranches.emplace_back(name);
   }

   TDataFrameBranch(const TDataFrameBranch &) = delete;

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   BranchNames GetTmpBranches() const { return fTmpBranches; }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   void *GetValue(unsigned int slot, int entry)
   {
      if (entry != fLastCheckedEntry[slot]) {
         // evaluate this filter, cache the result
         auto newValuePtr = GetValueHelper(BranchTypes_t(), TypeInd_t(), slot, entry);
         fLastResultPtr[slot] = newValuePtr;
         fLastCheckedEntry[slot] = entry;
      }
      return static_cast<void *>(fLastResultPtr[slot].get());
   }

   const std::type_info &GetTypeId() const { return typeid(RetType_t); }

   void CreateSlots(unsigned int nSlots)
   {
      fReaderValues.resize(nSlots);
      fLastCheckedEntry.resize(nSlots);
      fLastResultPtr.resize(nSlots);
   }

   bool CheckFilters(unsigned int slot, int entry)
   {
      // dummy call: it just forwards to the previous object in the chain
      return fPrevData->CheckFilters(slot, entry);
   }

   std::string GetName() const { return fName; }

   template <int... S, typename... BranchTypes>
   std::shared_ptr<RetType_t> GetValueHelper(Internal::TDFTraitsUtils::TTypeList<BranchTypes...>,
                                             Internal::TDFTraitsUtils::TStaticSeq<S...>,
                                             unsigned int slot, int entry)
   {
      auto valuePtr = std::make_shared<RetType_t>(fExpression(
         Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...));
      return valuePtr;
   }
};

class TDataFrameFilterBase {
public:
   virtual ~TDataFrameFilterBase() {}
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
};
using FilterBasePtr_t = std::shared_ptr<TDataFrameFilterBase>;
using FilterBaseVec_t = std::vector<FilterBasePtr_t>;

template <typename FilterF, typename PrevDataFrame>
class TDataFrameFilter final : public TDataFrameFilterBase {
   using BranchTypes_t = typename Internal::TDFTraitsUtils::TFunctionTraits<FilterF>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;

   FilterF fFilter;
   const BranchNames fBranches;
   const BranchNames fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   std::vector<Internal::TVBVec_t> fReaderValues = {};
   std::vector<int> fLastCheckedEntry = {-1};
   std::vector<int> fLastResult = {true}; // std::vector<bool> cannot be used in a MT context safely

public:
   TDataFrameFilter(FilterF f, const BranchNames &bl, std::shared_ptr<PrevDataFrame> pd)
      : fFilter(f), fBranches(bl), fTmpBranches(pd->GetTmpBranches()), fPrevData(pd.get()),
        fFirstData(pd->GetDataFrame()) { }

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   BranchNames GetTmpBranches() const { return fTmpBranches; }

   TDataFrameFilter(const TDataFrameFilter &) = delete;

   bool CheckFilters(unsigned int slot, int entry)
   {
      if (entry != fLastCheckedEntry[slot]) {
         if (!fPrevData->CheckFilters(slot, entry)) {
            // a filter upstream returned false, cache the result
            fLastResult[slot] = false;
         } else {
            // evaluate this filter, cache the result
            fLastResult[slot] = CheckFilterHelper(BranchTypes_t(), TypeInd_t(), slot, entry);
         }
         fLastCheckedEntry[slot] = entry;
      }
      return fLastResult[slot];
   }

   template <int... S, typename... BranchTypes>
   bool CheckFilterHelper(Internal::TDFTraitsUtils::TTypeList<BranchTypes...>,
                          Internal::TDFTraitsUtils::TStaticSeq<S...>,
                          unsigned int slot, int entry)
   {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      return fFilter(
         Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
   }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   void CreateSlots(unsigned int nSlots)
   {
      fReaderValues.resize(nSlots);
      fLastCheckedEntry.resize(nSlots);
      fLastResult.resize(nSlots);
   }
};

class TDataFrameImpl {

   Internal::ActionBaseVec_t fBookedActions;
   Details::FilterBaseVec_t fBookedFilters;
   std::map<std::string, TmpBranchBasePtr_t> fBookedBranches;
   std::vector<std::shared_ptr<bool>> fResPtrsReadiness;
   std::string fTreeName;
   TDirectory *fDirPtr = nullptr;
   TTree *fTree = nullptr;
   const BranchNames fDefaultBranches;
   // always empty: each object in the chain copies this list from the previous
   // and they must copy an empty list from the base TDataFrameImpl
   const BranchNames fTmpBranches;
   unsigned int fNSlots;
   // TDataFrameInterface<TDataFrameImpl> calls SetFirstData to set this to a
   // weak pointer to the TDataFrameImpl object itself
   // so subsequent objects in the chain can call GetDataFrame on TDataFrameImpl
   std::weak_ptr<TDataFrameImpl> fFirstData;

public:
   TDataFrameImpl(const std::string &treeName, TDirectory *dirPtr, const BranchNames &defaultBranches = {})
      : fTreeName(treeName), fDirPtr(dirPtr), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots()) { }

   TDataFrameImpl(TTree &tree, const BranchNames &defaultBranches = {}) : fTree(&tree), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots())
   { }

   TDataFrameImpl(const TDataFrameImpl &) = delete;

   void Run()
   {
#ifdef R__USE_IMT
      if (ROOT::IsImplicitMTEnabled()) {
         const auto fileName = fTree ? static_cast<TFile *>(fTree->GetCurrentFile())->GetName() : fDirPtr->GetName();
         const std::string    treeName = fTree ? fTree->GetName() : fTreeName;
         ROOT::TTreeProcessorMT tp(fileName, treeName);
         ROOT::TSpinMutex     slotMutex;
         std::map<std::thread::id, unsigned int> slotMap;
         unsigned int globalSlotIndex = 0;
         CreateSlots(fNSlots);
         tp.Process([this, &slotMutex, &globalSlotIndex, &slotMap](TTreeReader &r) -> void {
            const auto thisThreadID = std::this_thread::get_id();
            unsigned int slot;
            {
               std::lock_guard<ROOT::TSpinMutex> l(slotMutex);
               auto thisSlotIt = slotMap.find(thisThreadID);
               if (thisSlotIt != slotMap.end()) {
                  slot = thisSlotIt->second;
               } else {
                  slot = globalSlotIndex;
                  slotMap[thisThreadID] = slot;
                  ++globalSlotIndex;
               }
            }

            BuildAllReaderValues(r, slot);

            // recursive call to check filters and conditionally execute actions
            while (r.Next())
               for (auto &actionPtr : fBookedActions)
                  actionPtr->Run(slot, r.GetCurrentEntry());
         });
      } else {
#endif // R__USE_IMT
         TTreeReader r;
         if (fTree) {
            r.SetTree(fTree);
         } else {
            r.SetTree(fTreeName.c_str(), fDirPtr);
         }

         CreateSlots(1);
         BuildAllReaderValues(r, 0);

         // recursive call to check filters and conditionally execute actions
         while (r.Next())
            for (auto &actionPtr : fBookedActions)
               actionPtr->Run(0, r.GetCurrentEntry());
#ifdef R__USE_IMT
      }
#endif // R__USE_IMT

      // forget actions and "detach" the action result pointers marking them ready and forget them too
      fBookedActions.clear();
      for (auto readiness : fResPtrsReadiness) {
         *readiness.get() = true;
      }
      fResPtrsReadiness.clear();
   }

   // build reader values for all actions, filters and branches
   void BuildAllReaderValues(TTreeReader &r, unsigned int slot)
   {
      for (auto &ptr : fBookedActions) ptr->BuildReaderValues(r, slot);
      for (auto &ptr : fBookedFilters) ptr->BuildReaderValues(r, slot);
      for (auto &bookedBranch : fBookedBranches) bookedBranch.second->BuildReaderValues(r, slot);
   }

   // inform all actions filters and branches of the required number of slots
   void CreateSlots(unsigned int nSlots)
   {
      for (auto &ptr : fBookedActions) ptr->CreateSlots(nSlots);
      for (auto &ptr : fBookedFilters) ptr->CreateSlots(nSlots);
      for (auto &bookedBranch : fBookedBranches) bookedBranch.second->CreateSlots(nSlots);
   }

   std::weak_ptr<Details::TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   const BranchNames &GetDefaultBranches() const { return fDefaultBranches; }

   const BranchNames GetTmpBranches() const { return fTmpBranches; }

   TTree* GetTree() const {
      if (fTree) {
         return fTree;
      } else {
         auto treePtr = static_cast<TTree*>(fDirPtr->Get(fTreeName.c_str()));
         return treePtr;
      }
   }

   const TDataFrameBranchBase &GetBookedBranch(const std::string &name) const
   {
      return *fBookedBranches.find(name)->second.get();
   }

   void *GetTmpBranchValue(const std::string &branch, unsigned int slot, int entry)
   {
      return fBookedBranches.at(branch)->GetValue(slot, entry);
   }

   TDirectory *GetDirectory() const { return fDirPtr; }

   std::string GetTreeName() const { return fTreeName; }

   void SetFirstData(const std::shared_ptr<TDataFrameImpl>& sp) { fFirstData = sp; }

   void Book(Internal::ActionBasePtr_t actionPtr) { fBookedActions.emplace_back(actionPtr); }

   void Book(Details::FilterBasePtr_t filterPtr) { fBookedFilters.emplace_back(filterPtr); }

   void Book(TmpBranchBasePtr_t branchPtr) { fBookedBranches[branchPtr->GetName()] = branchPtr; }

   // dummy call, end of recursive chain of calls
   bool CheckFilters(int, unsigned int) { return true; }

   unsigned int GetNSlots() {return fNSlots;}

   template<typename T>
   TActionResultProxy<T> MakeActionResultPtr(std::shared_ptr<T> r)
   {
      auto readiness = std::make_shared<bool>(false);
      // since fFirstData is a weak_ptr to `this`, we are sure the lock succeeds
      auto df = fFirstData.lock();
      auto resPtr = TActionResultProxy<T>::MakeActionResultPtr(r, readiness, df);
      fResPtrsReadiness.emplace_back(readiness);
      return resPtr;
   }
};

} // end NS Details

} // end NS ROOT

// Functions and method implementations
namespace ROOT {
template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr,
                                            const BranchNames &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(treeName, dirPtr, defaultBranches))
{
   fProxiedPtr->SetFirstData(fProxiedPtr);
}

template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(TTree &tree, const BranchNames &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(tree, defaultBranches))
{
   fProxiedPtr->SetFirstData(fProxiedPtr);
}

template<typename T>
void TActionResultProxy<T>::TriggerRun()
{
   auto df = fFirstData.lock();
   if (!df) {
      throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
   }
   df->Run();
}

namespace Internal {
template <int S, typename T>
T &GetBranchValue(TVBPtr_t &readerValue, unsigned int slot, int entry, const std::string &branch,
                  std::weak_ptr<Details::TDataFrameImpl> df)
{
   if (readerValue == nullptr) {
      // temporary branch
      void *tmpBranchVal = df.lock()->GetTmpBranchValue(branch, slot, entry);
      return *static_cast<T *>(tmpBranchVal);
   } else {
      // real branch
      return **std::static_pointer_cast<TTreeReaderValue<T>>(readerValue);
   }
}

} // end NS Internal

} // end NS ROOT

// FIXME: need to rethink the printfunction

#endif // ROOT_TDATAFRAME
