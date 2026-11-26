#include "robinhood_price.h"

#include "bounded_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::providers {

namespace {
bool field_string(const codec::BoundedJsonDocument& doc, uint32_t object, const char* key,
                  char* output, size_t capacity) {
  size_t length = 0;
  return doc.DecodeString(doc.FindObjectValue(object, key), output, capacity, &length) && length;
}
bool field_text(const codec::BoundedJsonDocument& doc, uint32_t object, const char* key,
                char* output, size_t capacity, size_t maximum) {
  if (!field_string(doc, object, key, output, capacity)) return false;
  size_t begin = 0, end = strlen(output);
  while (begin < end && static_cast<unsigned char>(output[begin]) <= 0x20) ++begin;
  while (end > begin && static_cast<unsigned char>(output[end - 1]) <= 0x20) --end;
  if (end == begin || end - begin > maximum) return false;
  if (begin) memmove(output, output + begin, end - begin);
  output[end - begin] = '\0';
  return true;
}
bool decimal_string(const codec::BoundedJsonDocument& doc, uint32_t token, double* value) {
  char text[64]{}; size_t length = 0;
  if (!doc.DecodeString(token, text, sizeof(text), &length) || !length) return false;
  size_t begin = 0, end = length;
  while (begin < end && static_cast<unsigned char>(text[begin]) <= 0x20) ++begin;
  while (end > begin && static_cast<unsigned char>(text[end - 1]) <= 0x20) --end;
  if (begin == end) return false;
  size_t integer_digits = 0, decimals = 0; bool dot = false;
  for (size_t i = begin; i < end; ++i) {
    const char c = text[i];
    if (c == '.' && !dot) { dot = true; continue; }
    if (c < '0' || c > '9') return false;
    if (dot) ++decimals; else ++integer_digits;
  }
  if (!integer_digits || decimals > 18 || (dot && !decimals)) return false;
  text[end] = '\0'; char* parsed_end = nullptr;
  const double parsed = strtod(text + begin, &parsed_end);
  if (!parsed_end || parsed_end != text + end || !isfinite(parsed) || parsed <= 0) return false;
  *value = parsed; return true;
}
bool number_or_string(const codec::BoundedJsonDocument& doc, uint32_t token, double* value) {
  if (doc.Number(token, value)) return isfinite(*value);
  char text[64]{}; size_t length = 0;
  if (!doc.DecodeString(token, text, sizeof(text), &length) || !length) return false;
  char* end = nullptr; const double parsed = strtod(text, &end);
  if (!end || end == text || *end || !isfinite(parsed)) return false;
  *value = parsed; return true;
}
bool parse(const unsigned char* input, size_t length, codec::BoundedJsonToken* tokens,
           size_t capacity, codec::BoundedJsonDocument* doc, RobinhoodError* error) {
  if (!input || !length || !tokens || !capacity || !doc || !error) {
    if (error) *error = RobinhoodError::kInvalidArguments;
    return false;
  }
  if (length > codec::kBoundedJsonMaxInputBytes) { *error = RobinhoodError::kResponseTooLarge; return false; }
  codec::BoundedJsonError e = codec::BoundedJsonError::kNone;
  if (!doc->Parse(input, length, tokens, capacity, &e)) {
    *error = e == codec::BoundedJsonError::kTokenLimit ? RobinhoodError::kTokenLimit : RobinhoodError::kInvalidJson;
    return false;
  }
  *error = RobinhoodError::kNone; return true;
}
bool hex(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool evm(const char* s) {
  if (strlen(s) != 42 || s[0] != '0' || s[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i) if (!hex(static_cast<unsigned char>(s[i]))) return false;
  return true;
}
bool valid_iso_date(const char* s) {
  if (!s || strlen(s) < 20) return false;
  static const size_t digit_positions[] = {0,1,2,3,5,6,8,9,11,12,14,15,17,18};
  for (size_t i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); ++i)
    if (s[digit_positions[i]] < '0' || s[digit_positions[i]] > '9') return false;
  if (s[4]!='-'||s[7]!='-'||s[10]!='T'||s[13]!=':'||s[16]!=':') return false;
  const int month=(s[5]-'0')*10+s[6]-'0', day=(s[8]-'0')*10+s[9]-'0';
  const int hour=(s[11]-'0')*10+s[12]-'0', minute=(s[14]-'0')*10+s[15]-'0';
  const int second=(s[17]-'0')*10+s[18]-'0';
  if(month<1||month>12||day<1||day>31||hour>23||minute>59||second>59) return false;
  size_t i=19;
  if(s[i]=='.'){++i;size_t start=i;while(s[i]>='0'&&s[i]<='9')++i;if(i==start)return false;}
  if(s[i]=='Z') return s[i+1]=='\0';
  if((s[i]=='+'||s[i]=='-') && s[i+1]>='0'&&s[i+1]<='2'&&s[i+2]>='0'&&s[i+2]<='9'&&
     s[i+3]==':'&&s[i+4]>='0'&&s[i+4]<='5'&&s[i+5]>='0'&&s[i+5]<='9'&&s[i+6]=='\0') return true;
  return false;
}
bool valid_uid(const char* s) {
  if (strlen(s) != 66 || s[0] != '0' || s[1] != 'x') return false;
  for (size_t i = 2; i < 66; ++i) if (!hex(static_cast<unsigned char>(s[i]))) return false;
  return true;
}
}

RobinhoodError ExtractRobinhoodAssets(const unsigned char* input, size_t input_length,
    codec::BoundedJsonToken* tokens, size_t token_capacity, RobinhoodAssetCatalog* output) {
  if (!output) return RobinhoodError::kInvalidArguments;
  *output = RobinhoodAssetCatalog{};
  codec::BoundedJsonDocument doc; RobinhoodError error{};
  if (!parse(input, input_length, tokens, token_capacity, &doc, &error)) return error;
  const uint32_t rows = doc.FindObjectValue(doc.root(), "assets");
  const auto* array = doc.token(rows);
  if (!array || array->type != codec::BoundedJsonType::kArray) return RobinhoodError::kInvalidJson;
  for (uint32_t row = array->first_child; row != codec::kBoundedJsonNoToken;) {
    const auto* row_token = doc.token(row); if (!row_token) return RobinhoodError::kInvalidJson;
    const uint32_t next = row_token->next_sibling;
    if (row_token->type == codec::BoundedJsonType::kObject) {
      char symbol[kRobinhoodMaxSymbolBytes + 1]{}, name[161]{}, uid[69]{};
      double multiplier = 0;
      const bool valid_base = field_text(doc,row,"tokenSymbol",symbol,sizeof(symbol),32) &&
        field_text(doc,row,"tokenName",name,sizeof(name),160) && field_text(doc,row,"id",uid,sizeof(uid),68) &&
        valid_uid(uid) && decimal_string(doc,doc.FindObjectValue(row,"currentMultiplier"),&multiplier);
      uint32_t deployments = doc.FindObjectValue(row,"deployments");
      const auto* deps = doc.token(deployments);
      bool matched = false; char address[43]{};
      if (valid_base && deps && deps->type == codec::BoundedJsonType::kArray) {
        for (uint32_t dep=deps->first_child;dep!=codec::kBoundedJsonNoToken;) {
          const auto* dt=doc.token(dep); if(!dt) break; const uint32_t dn=dt->next_sibling;
          double chain=0; char candidate[43]{};
          if(dt->type==codec::BoundedJsonType::kObject && number_or_string(doc,doc.FindObjectValue(dep,"chainId"),&chain) && chain==4663 &&
             field_string(doc,dep,"contractAddress",candidate,sizeof(candidate)) && evm(candidate)) { memcpy(address,candidate,sizeof(address)); matched=true; break; }
          dep=dn;
        }
      }
      if (matched) {
        bool duplicate=false;
        for(size_t i=0;i<output->count;++i) if(strcasecmp(output->assets[i].address,address)==0){duplicate=true;break;}
        if(duplicate) { row=next; continue; }
        if(output->count==kRobinhoodMaxAssets) return RobinhoodError::kCatalogLimit;
        RobinhoodAsset& asset=output->assets[output->count++]; memcpy(asset.address,address,sizeof(address));
        for(char* p=asset.address;*p;++p) if(*p>='A'&&*p<='F') *p=static_cast<char>(*p+32);
        memcpy(asset.symbol,symbol,strlen(symbol)+1); asset.multiplier=multiplier;
      }
    }
    row=next;
  }
  return RobinhoodError::kNone;
}

bool BuildRobinhoodPricePath(const char* symbol, char* output, size_t capacity) {
  if (!symbol || !*symbol || !output || capacity < 2) return false;
  size_t n=0; const char prefix[]="/rhj/prices/";
  if(sizeof(prefix)-1>=capacity) return false;
  memcpy(output,prefix,sizeof(prefix)-1); n=sizeof(prefix)-1;
  static const char hexes[]="0123456789ABCDEF";
  for(const unsigned char* p=reinterpret_cast<const unsigned char*>(symbol);*p;++p) {
    const bool safe=(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-_.!~*'()",*p);
    const size_t add=safe?1:3; if(add>=capacity-n) return false;
    if(safe) output[n++]=static_cast<char>(*p); else {output[n++]='%';output[n++]=hexes[*p>>4];output[n++]=hexes[*p&15];}
  }
  output[n]='\0'; return true;
}

bool BuildRobinhoodRequest(const config::ProviderRuntimeConfig& config, const char* path,
    http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (!request || !path || strnlen(path, kRobinhoodPricePathBytes) >= kRobinhoodPricePathBytes ||
      path[0] != '/' || !config.ca_bundle_configured || !config.ca_bundle_path[0]) return false;
  request->method = http::OutboundMethod::kGet;
  request->host = "api.robinhood.com";
  request->port = "443";
  request->path = path;
  request->ca_file = config.ca_bundle_path;
  request->timeout_ms = 10000;
  request->max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  return true;
}

RobinhoodError ExtractRobinhoodQuote(const unsigned char* input, size_t input_length,
    const RobinhoodAsset& requested, codec::BoundedJsonToken* tokens, size_t token_capacity,
    double* midpoint_usd) {
  if(!midpoint_usd||!evm(requested.address)||!requested.symbol[0]||requested.multiplier<=0) return RobinhoodError::kInvalidArguments;
  *midpoint_usd=0; codec::BoundedJsonDocument doc; RobinhoodError error{};
  if(!parse(input,input_length,tokens,token_capacity,&doc,&error)) return error;
  const auto* quotes=doc.token(doc.FindObjectValue(doc.root(),"quotes"));
  if(!quotes||quotes->type!=codec::BoundedJsonType::kArray) return RobinhoodError::kInvalidQuote;
  for(uint32_t row=quotes->first_child;row!=codec::kBoundedJsonNoToken;) {
    const auto* rt=doc.token(row); if(!rt) return RobinhoodError::kInvalidJson; const uint32_t next=rt->next_sibling;
    if(rt->type==codec::BoundedJsonType::kObject) {
      char symbol[33]{}; if(field_string(doc,row,"tokenSymbol",symbol,sizeof(symbol))&&strcmp(symbol,requested.symbol)==0) {
        uint32_t deps=doc.FindObjectValue(row,"deployments"); const auto* da=doc.token(deps); bool match=false;
        if(da&&da->type==codec::BoundedJsonType::kArray) for(uint32_t d=da->first_child;d!=codec::kBoundedJsonNoToken;) {
          const auto* dt=doc.token(d); if(!dt) break; const uint32_t dn=dt->next_sibling; double chain=0; char address[43]{};
          if(dt->type==codec::BoundedJsonType::kObject&&number_or_string(doc,doc.FindObjectValue(d,"chainId"),&chain)&&chain==4663&&field_string(doc,d,"contractAddress",address,sizeof(address))&&strcasecmp(address,requested.address)==0){match=true;break;} d=dn;
        }
        if (!match) { row=next; continue; }
        uint32_t currency=doc.FindObjectValue(row,"currency");
        char generated[96]{};
        if(!doc.StringEquals(currency,"USD") || !field_string(doc,row,"generatedAt",generated,sizeof(generated)) || !valid_iso_date(generated))
          return RobinhoodError::kInvalidQuote;
        double bid=0,ask=0; if(number_or_string(doc,doc.FindObjectValue(row,"bid"),&bid)&&number_or_string(doc,doc.FindObjectValue(row,"ask"),&ask)&&bid>0&&ask>0&&ask>=bid) {
          bid*=requested.multiplier; ask*=requested.multiplier; const double mid=(bid+ask)/2;
          if(isfinite(mid)&&mid>0){*midpoint_usd=mid;return RobinhoodError::kNone;}
        }
        return RobinhoodError::kInvalidQuote;
      }
    }
    row=next;
  }
  return RobinhoodError::kInvalidQuote;
}

}  // namespace fortis::providers
