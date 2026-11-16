// Image AI through OpenRouter (https://openrouter.ai), with the user's own API key.
// Nothing here runs unless the user saves a key and presses Generate: then the image region
// they chose (and their prompt) goes to OpenRouter and the model provider they picked.
// The key is kept encrypted for the Windows user (DPAPI) in %LOCALAPPDATA%\CircleRoom.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace orouter {

struct Model {
  std::string id, name;
  bool free = false;                      // costs nothing right now (":free" or zero pricing)
  bool edits = false;                     // takes input images (edit a photo, not only text-to-image)
  std::vector<std::string> aspects;       // supported aspect_ratio values ("16:9"...), may be empty
  std::vector<std::string> resolutions;   // supported resolution values ("1K"...), may be empty
  std::vector<std::string> formats;       // supported output_format values
  double outCost = -1;                    // USD per picture made (-1 unknown)
  double inCost = 0;                      // USD per picture sent in (edits send two)
  double out2K = -1;                      // USD per picture at 2K
  bool approx = false;                    // estimated: the provider bills per token or megapixel
  bool normTok = false;                   // priced in OpenRouter image tokens (4175 per 1K picture)
};

// ---- API key (encrypted for this Windows user)
bool loadKey(std::string& key);
bool saveKey(const std::string& key, std::string& err);
void clearKey();
bool plausibleKey(const std::string& key);  // "sk-or-..." and no spaces

// ---- models
// Downloads both model lists (no key needed) and merges them: capabilities from the image API,
// prices and ":free" variants from the general list. Sorted: free first, then by name.
bool fetchModels(std::vector<Model>& out, std::string& err);
bool parseModels(const std::string& imagesJson, const std::string& pricesJson, std::vector<Model>& out);
bool loadModelCache(std::vector<Model>& out, long long* ageSec = nullptr);
void saveModelCache(const std::vector<Model>& m);
std::string loadChosenModel();
void saveChosenModel(const std::string& id);
// The supported aspect ratio closest to w/h (empty when the model lists none).
std::string nearestAspect(const Model& m, float ratio, float* exact = nullptr);
// Reads one model's endpoints answer (/api/v1/images/models/<id>/endpoints) into a price per
// picture: the cheapest endpoint; token prices assume ~1290 tokens per 1K picture made and
// ~560 per picture sent, megapixel prices ~1 MP.
bool parseEndpointPrice(const std::string& json, Model& m);
// Cost of one result: "$0.039", "~$0.04" (estimate) or "" (unknown). edit: adds the two
// pictures sent (photo + mask). hiRes: 2K. paid: the price is what this model really cost before.
std::string priceText(const Model& m, bool edit, bool hiRes, bool* paid = nullptr);
// What a result really cost (from OpenRouter's answer), remembered per model, task and size.
double paidCost(const std::string& id, bool edit, bool hiRes);
void rememberCost(const std::string& id, bool edit, bool hiRes, double usd);

// ---- generation
struct Input {
  std::vector<uint8_t> bytes;  // encoded image
  std::string mime;            // "image/jpeg" / "image/png"
};
struct Request {
  std::string model, prompt, aspect, resolution, format;
  std::vector<Input> images;
};
struct Result {
  std::vector<uint8_t> bytes;  // encoded image
  std::string mime;
  double cost = -1;  // USD, when OpenRouter reports it
};
// Blocking (call from a worker thread). cancel() from any thread aborts it.
bool generate(const std::string& key, const Request& rq, Result& out, std::string& err);
void cancel();

void openKeysPage();                  // openrouter.ai/keys in the browser
void wipe(void* p, size_t n);         // clears a buffer that held the key

// pieces for the self-test
std::string buildRequestJson(const Request& rq);
bool parseResult(const std::string& body, Result& out, std::string& err);
std::string base64(const uint8_t* d, size_t n);
bool unbase64(const std::string& s, std::vector<uint8_t>& out);

}  // namespace orouter
