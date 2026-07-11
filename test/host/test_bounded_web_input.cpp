#include <Arduino.h>

#include "lib/BoundedWebInput.h"
#include "test_support.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>

using namespace bp_web;

static bool gDenyAllocations = false;
static size_t gDeniedAllocationCalls = 0;

void* operator new(std::size_t size) {
  if (gDenyAllocations) {
    ++gDeniedAllocationCalls;
    throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

static bool objectContains(const void* object, size_t objectSize,
                           const char* needle) {
  const auto* bytes = static_cast<const uint8_t*>(object);
  const size_t needleLength = std::strlen(needle);
  if (needleLength == 0 || needleLength > objectSize) return false;
  for (size_t offset = 0; offset + needleLength <= objectSize; ++offset) {
    if (std::memcmp(bytes + offset, needle, needleLength) == 0) return true;
  }
  return false;
}

static bool validateBody(BoundedFormValidator& validator, const char* body) {
  return validator.validate(nullptr, 0, body, std::strlen(body));
}

static void testIngressRetainsOnlyUnreadSocketBytes() {
  BoundedIngressBuffer buffer;
  CHECK_EQ(buffer.writableCapacity(), BoundedIngressBuffer::kCapacity,
           "empty ingress exposes one fixed socket-read budget");
  static const char wire[] = "headers\r\n\r\nbody-secret";
  std::memcpy(buffer.writableData(), wire, sizeof(wire) - 1);
  CHECK_TRUE(buffer.commit(sizeof(wire) - 1),
             "socket read length commits within fixed capacity");
  CHECK_EQ(buffer.writableCapacity(), 0UL,
           "pending bytes cannot be overwritten by another socket read");
  CHECK_TRUE(buffer.writableData() == nullptr,
             "pending ingress exposes no writable pointer");
  CHECK_EQ(buffer.length(), sizeof(wire) - 1,
           "committed socket bytes are all pending");
  CHECK_TRUE(buffer.consume(11),
             "parser consumption advances only acknowledged prefix");
  CHECK_EQ(buffer.length(), sizeof(wire) - 1 - 11,
           "unconsumed body remains buffered for policy decision");
  CHECK_TRUE(std::memcmp(buffer.data(), "body-secret", 11) == 0,
             "unread body bytes retain exact order");
  CHECK_TRUE(!objectContains(&buffer, sizeof(buffer), "headers"),
             "consumed header prefix is wiped immediately");
  CHECK_TRUE(buffer.consume(buffer.length()),
             "remaining body can be acknowledged after policy");
  CHECK_EQ(buffer.length(), 0UL, "fully consumed ingress becomes empty");
  CHECK_EQ(buffer.writableCapacity(), BoundedIngressBuffer::kCapacity,
           "empty ingress permits the next bounded socket read");
  CHECK_TRUE(!objectContains(&buffer, sizeof(buffer), "body-secret"),
             "consumed body secret is wiped immediately");
}

static void testIngressInvalidProgressAndLifecycleWipe() {
  BoundedIngressBuffer buffer;
  std::memset(buffer.writableData(), 'x', BoundedIngressBuffer::kCapacity);
  CHECK_TRUE(!buffer.commit(BoundedIngressBuffer::kCapacity + 1),
             "socket length beyond capacity fails closed");
  CHECK_EQ(buffer.length(), 0UL,
           "invalid socket length leaves no readable bytes");

  static const char secret[] = "pending-bootstrap-token";
  std::memcpy(buffer.writableData(), secret, sizeof(secret) - 1);
  CHECK_TRUE(buffer.commit(sizeof(secret) - 1),
             "wipe fixture commits sensitive bytes");
  CHECK_TRUE(!buffer.consume(sizeof(secret)),
             "progress beyond pending length fails closed");
  CHECK_EQ(buffer.length(), 0UL,
           "invalid parser progress clears pending ingress");
  CHECK_TRUE(!objectContains(&buffer, sizeof(buffer), secret),
             "invalid progress wipes sensitive ingress");

  std::memcpy(buffer.writableData(), secret, sizeof(secret) - 1);
  (void)buffer.commit(sizeof(secret) - 1);
  buffer.clear();
  CHECK_TRUE(!objectContains(&buffer, sizeof(buffer), secret),
             "explicit disconnect clear wipes ingress");

  alignas(BoundedIngressBuffer)
    uint8_t storage[sizeof(BoundedIngressBuffer)];
  auto* placed = ::new (static_cast<void*>(storage)) BoundedIngressBuffer();
  std::memcpy(placed->writableData(), secret, sizeof(secret) - 1);
  (void)placed->commit(sizeof(secret) - 1);
  placed->~BoundedIngressBuffer();
  CHECK_TRUE(!objectContains(storage, sizeof(storage), secret),
             "ingress destructor wipes pending bytes");
}

static void testStrictFormValidationAndParameterPollution() {
  BoundedFormValidator validator;
  static const char query[] = "rescan=1";
  static const char body[] =
    "ssid=manual&manual_ssid=Clinic+West&password=p%40ss";
  CHECK_TRUE(validator.validate(query, sizeof(query) - 1,
                                body, sizeof(body) - 1),
             "valid query and form body pass strict decoding");
  CHECK_EQ(validator.fieldCount(), 4UL,
           "combined query/body field count is explicit");
  CHECK_STR(validator.key(0), "rescan",
            "validated query key is available for bounded materialization");
  CHECK_STR(validator.value(0), "1",
            "validated query value is decoded exactly");
  CHECK_STR(validator.key(2), "manual_ssid",
            "form key order is preserved after query fields");
  CHECK_STR(validator.value(2), "Clinic West",
            "plus decodes to space for handler materialization");
  CHECK_STR(validator.value(3), "p@ss",
            "percent escapes decode once for handler materialization");
  CHECK_EQ(validator.keyLength(2), std::strlen("manual_ssid"),
           "decoded key length is explicit");
  CHECK_EQ(validator.valueLength(3), std::strlen("p@ss"),
           "decoded value length is explicit");
  CHECK_TRUE(validateBody(validator, "password="),
             "empty form value remains valid");
  CHECK_TRUE(validateBody(validator, "ssid=%E4%B8%AD"),
             "percent-encoded UTF-8 value remains valid");

  CHECK_TRUE(!validator.validate("ssid=a", std::strlen("ssid=a"),
                                 "ssid=b", std::strlen("ssid=b")),
             "duplicate key across query and body is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=a&%73sid=b"),
             "encoded duplicate key is rejected after decoding");
  CHECK_TRUE(!validateBody(validator, "=value"),
             "empty form key is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid"),
             "field without equals delimiter is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=a=b"),
             "raw second equals delimiter is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=a&&x=1"),
             "empty field between ampersands is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=%"),
             "truncated percent escape is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=%0G"),
             "non-hex percent escape is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=%00"),
             "encoded NUL in value is rejected");
  CHECK_TRUE(!validateBody(validator, "ssid=%7f"),
             "encoded DEL in value is rejected");
  CHECK_TRUE(!validateBody(validator, "bad%2fkey=x"),
             "decoded punctuation in key is rejected");
  CHECK_EQ(validator.fieldCount(), 0UL,
           "failed validation exposes no stale field metadata");
  CHECK_STR(validator.value(0), "",
            "failed validation wipes prior decoded values");
}

static void testFormCapsAndAllocationContract() {
  BoundedFormValidator validator;
  std::string seventeen;
  for (int i = 0; i < 17; ++i) {
    if (!seventeen.empty()) seventeen += '&';
    seventeen += "k" + std::to_string(i) + "=v";
  }
  CHECK_TRUE(!validator.validate(nullptr, 0, seventeen.c_str(),
                                 seventeen.size()),
             "more than sixteen fields is rejected");

  const std::string longKey(33, 'k');
  const std::string longKeyForm = longKey + "=v";
  CHECK_TRUE(!validator.validate(nullptr, 0, longKeyForm.c_str(),
                                 longKeyForm.size()),
             "decoded key cap is enforced");
  const std::string longValue = "ssid=" + std::string(129, 'v');
  CHECK_TRUE(!validator.validate(nullptr, 0, longValue.c_str(),
                                 longValue.size()),
             "decoded value cap is enforced");

  static_assert(!std::is_copy_constructible<BoundedIngressBuffer>::value,
                "ingress cannot copy sensitive pending bytes");
  static_assert(!std::is_copy_assignable<BoundedIngressBuffer>::value,
                "ingress cannot copy-assign sensitive pending bytes");
  static const char fixedQuery[] = "rescan=1";
  static const char fixedBody[] = "ssid=manual&password=p%40ss";
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    (void)validator.validate(fixedQuery, sizeof(fixedQuery) - 1,
                             fixedBody, sizeof(fixedBody) - 1);
    BoundedIngressBuffer ingress;
    std::memcpy(ingress.writableData(), fixedBody, sizeof(fixedBody) - 1);
    (void)ingress.commit(sizeof(fixedBody) - 1);
    (void)ingress.consume(ingress.length());
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "web input boundary works with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "web input boundary performs no allocation attempt");

  CHECK_TRUE(validator.validate(nullptr, 0, "password=secret42", 17),
             "clear fixture validates sensitive value");
  CHECK_TRUE(objectContains(&validator, sizeof(validator), "secret42"),
             "decoded sensitive value exists before explicit clear");
  validator.clear();
  CHECK_EQ(validator.fieldCount(), 0UL,
           "explicit validator clear removes field metadata");
  CHECK_TRUE(!objectContains(&validator, sizeof(validator), "secret42"),
             "explicit validator clear wipes decoded values");

  uint32_t random = 0x9e3779b9U;
  char fuzz[65] = {};
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    random ^= random << 13U;
    random ^= random >> 17U;
    random ^= random << 5U;
    const size_t length = random % 65U;
    for (size_t i = 0; i < length; ++i) {
      random ^= random << 13U;
      random ^= random >> 17U;
      random ^= random << 5U;
      fuzz[i] = static_cast<char>(random & 0xffU);
    }
    const bool valid = validator.validate(nullptr, 0, fuzz, length);
    CHECK_TRUE(!valid || validator.fieldCount() <=
                          BoundedFormValidator::kMaxFields,
               "fuzzed accepted form always respects fixed field cap");
  }
}

int main() {
  testIngressRetainsOnlyUnreadSocketBytes();
  testIngressInvalidProgressAndLifecycleWipe();
  testStrictFormValidationAndParameterPollution();
  testFormCapsAndAllocationContract();
  return testReport();
}
