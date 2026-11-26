#pragma once
#include "news_store.h"
namespace fortis::news {
struct FeedQuery {
  Topic topics[64];
  size_t topic_count = 0;
  bool has_topics = false;
  size_t limit = 40;
  bool has_from = false;
  int64_t from_ms = 0;
  char before_id[27]{};
  bool valid = true;
};
class FeedQueryService {
 public:
  explicit FeedQueryService(NewsRepository& repository) : repository_(repository) {}
  bool Feed(const FeedQuery& query, int64_t now, Arena* arena, OutBuffer& output) const;
  bool Detail(const char* id, int64_t now, Arena* arena, OutBuffer& output) const;
 private:
  NewsRepository& repository_;
};
}  // namespace fortis::news
