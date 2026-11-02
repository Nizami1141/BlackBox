#include "../include/aks_face_selection.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    JsonDocument doc;
    auto select = [&](const char *json) {
        assert(!deserializeJson(doc, json));
        return aks::selectFace(doc["faces"].as<JsonArrayConst>());
    };

    // An unknown bystander may precede the recognized person. Every field
    // needed by the LCD and cargo request must come from the chosen person.
    auto result = select(R"({"faces":[
        {"match":false,"log_id":99,"similarity":0.99},
        {"match":true,"log_id":12,"user":{"first_name":"Ali","last_name":"Test","similarity":0.8}}
    ]})");
    assert(!result.error && result.matches == 1);
    assert(result.face["log_id"].as<int>() == 12);
    assert(!strcmp(result.face["user"]["first_name"].as<const char *>(), "Ali"));
    assert(!strcmp(result.face["user"]["last_name"].as<const char *>(), "Test"));

    result = select(R"({"faces":[
        {"match":true,"log_id":10,"user":{"similarity":0.6}},
        {"match":false},
        {"match":true,"log_id":20,"user":{"similarity":0.9}}
    ]})");
    assert(!result.error && result.matches == 2 && result.face["log_id"] == 20);

    // Highest confidence can also be the first face; do not just take last.
    result = select(R"({"faces":[
        {"match":true,"log_id":20,"similarity":0.9},
        {"match":true,"log_id":10,"similarity":0.6}
    ]})");
    assert(!result.error && result.face["log_id"] == 20);

    result = select(R"({"faces":[
        {"match":true,"log_id":10,"score":0.8},
        {"match":true,"log_id":20,"score":0.8}
    ]})");
    assert(!result.error && result.face["log_id"] == 10); // stable tie

    result = select(R"({"faces":[{"match":true,"log_id":10},{"match":true,"log_id":20}]})");
    assert(!result.error && result.face["log_id"] == 10); // no confidence
    result = select(R"({"faces":[{"match":true,"log_id":10},{"match":true,"log_id":20,"similarity":0.7}]})");
    assert(!result.error && result.face["log_id"] == 20);

    result = select(R"({"faces":[{"match":false,"message":"unknown"},{"match":false}]})");
    assert(!result.error && !result.matches && result.face["match"] == false);
    result = select(R"({"faces":[]})");
    assert(!result.error && !result.matches && result.face.isNull());
    result = select(R"({"faces":[{"match":true,"log_id":123}]})");
    assert(!result.error && result.matches == 1 && result.face["log_id"] == 123);

    const char *invalid[] = {
        R"({})", R"({"faces":{}})", R"({"faces":[null]})",
        R"({"faces":[{"match":"true","log_id":12}]})",
        R"({"faces":[{"match":true}]})",
        R"({"faces":[{"match":true,"log_id":0}]})",
        R"({"faces":[{"match":true,"log_id":"12"}]})",
        R"({"faces":[{"match":true,"log_id":12},{"match":true,"log_id":0}]})"
    };
    for (const char *json : invalid) assert(select(json).error);
    std::cout << "AKS: multiple faces, best match, name/session consistency, denials and malformed replies passed\n";
}
