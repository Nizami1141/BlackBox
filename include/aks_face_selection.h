#pragma once

#include <ArduinoJson.h>
#include <cmath>

namespace aks {

struct FaceSelection {
    JsonObjectConst face;
    const char *error = nullptr;
    unsigned matches = 0;
    double similarity = -1;
};

inline double faceSimilarity(JsonObjectConst face) {
    const JsonVariantConst values[] = {
        face["user"]["similarity"], face["similarity"], face["score"]
    };
    for (JsonVariantConst value : values) {
        if (value.is<double>()) {
            const double score = value.as<double>();
            if (std::isfinite(score) && score >= 0 && score <= 1) return score;
        }
    }
    return -1;
}

// Keep the selected person's name and session in the same JSON object.
// Ties (including absent similarity) preserve the server's response order.
inline FaceSelection selectFace(JsonArrayConst faces) {
    FaceSelection result;
    if (faces.isNull()) {
        result.error = "неверный ответ распознавания: faces отсутствует или не массив";
        return result;
    }
    for (JsonVariantConst entry : faces) {
        if (!entry.is<JsonObjectConst>()) {
            result.error = "неверный ответ распознавания: лицо не объект";
            return result;
        }
        JsonObjectConst face = entry.as<JsonObjectConst>();
        if (!face["match"].is<bool>()) {
            result.error = "неверный ответ распознавания: match отсутствует или не true/false";
            return result;
        }
        if (!face["match"].as<bool>()) continue;
        if (!face["log_id"].is<int>() || face["log_id"].as<int>() <= 0) {
            result.error = "сервер не выдал действительный log_id";
            return result;
        }
        const double score = faceSimilarity(face);
        if (result.matches == 0 || score > result.similarity) {
            result.face = face;
            result.similarity = score;
        }
        ++result.matches;
    }
    // No recognized people: retain the normal denial message, never accept
    // an unmatched face merely because it has a high similarity value.
    if (result.matches == 0 && faces.size())
        result.face = faces[0].as<JsonObjectConst>();
    return result;
}

}  // namespace aks
