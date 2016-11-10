#pragma once

#include "coding/bit_streams.hpp"
#include "coding/elias_coder.hpp"
#include "coding/file_writer.hpp"
#include "coding/reader.hpp"
#include "coding/varint.hpp"
#include "coding/write_to_sink.hpp"

#include "base/assert.hpp"
#include "base/bits.hpp"

#include "std/algorithm.hpp"
#include "std/string.hpp"
#include "std/vector.hpp"

namespace routing
{
/// \brief Restriction to modify road graph.
struct Restriction
{
  static uint32_t const kInvalidFeatureId;

  /// \brief Types of road graph restrictions.
  /// \note Despite the fact more that 10 restriction tags are present in osm all of them
  /// could be split into two categories.
  /// * no_left_turn, no_right_turn, no_u_turn and so on go to "No" category.
  /// * only_left_turn, only_right_turn and so on go to "Only" category.
  /// That's enough to rememeber if
  /// * there's only way to pass the junction is driving along the restriction (Only)
  /// * driving along the restriction is prohibited (No)
  enum class Type
  {
    No,    // Going according such restriction is prohibited.
    Only,  // Only going according such restriction is permitted
  };

  Restriction(Type type, vector<uint32_t> const & links) : m_featureIds(links), m_type(type) {}

  bool IsValid() const;
  bool operator==(Restriction const & restriction) const;
  bool operator<(Restriction const & restriction) const;

  // Links of the restriction in feature ids term.
  vector<uint32_t> m_featureIds;
  Type m_type;
};

using RestrictionVec = vector<Restriction>;

string ToString(Restriction::Type const & type);
string DebugPrint(Restriction::Type const & type);
string DebugPrint(Restriction const & restriction);
}  // namespace routing

namespace feature
{
struct RoutingHeader
{
  RoutingHeader() { Reset(); }

  template <class Sink>
  void Serialize(Sink & sink) const
  {
    WriteToSink(sink, m_version);
    WriteToSink(sink, m_reserved);
    WriteToSink(sink, m_noRestrictionCount);
    WriteToSink(sink, m_onlyRestrictionCount);
  }

  template <class Source>
  void Deserialize(Source & src)
  {
    m_version = ReadPrimitiveFromSource<uint16_t>(src);
    m_reserved = ReadPrimitiveFromSource<uint16_t>(src);
    m_noRestrictionCount = ReadPrimitiveFromSource<uint32_t>(src);
    m_onlyRestrictionCount = ReadPrimitiveFromSource<uint32_t>(src);
  }

  void Reset()
  {
    m_version = 0;
    m_reserved = 0;
    m_noRestrictionCount = 0;
    m_onlyRestrictionCount = 0;
  }

  uint16_t m_version;
  uint16_t m_reserved;
  uint32_t m_noRestrictionCount;
  uint32_t m_onlyRestrictionCount;
};

static_assert(sizeof(RoutingHeader) == 12, "Wrong header size of routing section.");

class RestrictionSerializer
{
public:
  template <class Sink>
  static void Serialize(routing::RestrictionVec::const_iterator begin,
                        routing::RestrictionVec::const_iterator firstOnlyIt,
                        routing::RestrictionVec::const_iterator end, Sink & sink)
  {
    SerializeSingleType(begin, firstOnlyIt, sink);
    SerializeSingleType(firstOnlyIt, end, sink);
  }

  template <class Source>
  static void Deserialize(RoutingHeader const & header, routing::RestrictionVec & restrictions,
                          Source & src)
  {
    DeserializeSingleType(routing::Restriction::Type::No, header.m_noRestrictionCount,
                          restrictions, src);
    DeserializeSingleType(routing::Restriction::Type::Only, header.m_onlyRestrictionCount,
                          restrictions, src);
  }

private:
  static uint32_t const kDefaultFeatureId;

  /// \brief Serializes a range of restrictions form |begin| to |end| to |sink|.
  /// \param begin is an iterator to the first item to serialize.
  /// \param end is an iterator to the element after the last element to serialize.
  /// \note All restrictions should have the same type.
  template <class Sink>
  static void SerializeSingleType(routing::RestrictionVec::const_iterator begin,
                                  routing::RestrictionVec::const_iterator end, Sink & sink)
  {
    if (begin == end)
      return;

    CHECK(is_sorted(begin, end), ());
    routing::Restriction::Type const type = begin->m_type;

    uint32_t prevFisrtLinkFeatureId = 0;
    for (auto it = begin; it != end; ++it)
    {
      CHECK_EQUAL(type, it->m_type, ());

      routing::Restriction const & restriction = *it;
      CHECK(restriction.IsValid(), ());
      CHECK_LESS(1, restriction.m_featureIds.size(), ("No meaning in zero or one link restrictions."));

      BitWriter<FileWriter> bits(sink);
      coding::DeltaCoder::Encode(bits, restriction.m_featureIds.size() - 1 /* link number is two or more */);
      uint32_t prevLinkFeatureId = prevFisrtLinkFeatureId;
      for (size_t i = 0; i < restriction.m_featureIds.size(); ++i)
      {
        uint32_t const delta = bits::ZigZagEncode(static_cast<int32_t>(restriction.m_featureIds[i]) -
                                                  static_cast<int32_t>(prevLinkFeatureId));
        coding::DeltaCoder::Encode(bits, delta + 1 /* making it greater than zero */);
        prevLinkFeatureId = restriction.m_featureIds[i];
      }
      prevFisrtLinkFeatureId = restriction.m_featureIds[0];
    }
  }

  template <class Source>
  static bool DeserializeSingleType(routing::Restriction::Type type, uint32_t count,
                                    routing::RestrictionVec & restrictions, Source & src)
  {
    uint32_t prevFisrtLinkFeatureId = 0;
    for (size_t i = 0; i < count; ++i)
    {
      BitReader<Source> bits(src);
      uint32_t const biasedLinkNumber = coding::DeltaCoder::Decode(bits);
      if (biasedLinkNumber == 0)
      {
        LOG(LERROR, ("Decoded link restriction number is zero."));
        return false;
      }
      size_t const linkNumber = biasedLinkNumber + 1 /* link number is two or more */;

      routing::Restriction restriction(type,  {} /* links */);
      restriction.m_featureIds.resize(linkNumber);
      uint32_t prevLinkFeatureId = prevFisrtLinkFeatureId;
      for (size_t i = 0; i < linkNumber; ++i)
      {
        uint32_t const biasedDelta = coding::DeltaCoder::Decode(bits);
        if (biasedDelta == 0)
        {
          LOG(LERROR, ("Decoded link restriction feature id delta is zero."));
          return false;
        }
        uint32_t const delta = biasedDelta - 1;
        restriction.m_featureIds[i] = static_cast<uint32_t>(
            bits::ZigZagDecode(delta) + prevLinkFeatureId);
        prevLinkFeatureId = restriction.m_featureIds[i];
      }

      prevFisrtLinkFeatureId = restriction.m_featureIds[0];
      restrictions.push_back(restriction);
    }
    return true;
  }
};
}  // namespace feature
