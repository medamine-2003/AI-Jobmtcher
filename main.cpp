#include <iostream>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include "sqlite3.h"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;
using namespace httplib;

struct JobMatch {
    int job_id;
    std::string job_title;
    double score;
    std::set<int> missing_skills;
    bool has_required_skills;
};

sqlite3* db;

static int collect_single_id_callback(void* data, int argc, char** argv, char** colNames) {
    std::vector<int>* ids = static_cast<std::vector<int>*>(data);
    if (argc < 1 || argv[0] == nullptr) return 0;
    try {
        ids->push_back(std::stoi(argv[0]));
    } catch (...) {
        return 1;
    }
    return 0;
}

static int get_name_callback(void* data, int argc, char** argv, char** colNames) {
    std::string* name = static_cast<std::string*>(data);
    *name = (argc >= 2 && argv[1]) ? argv[1] : "Unknown";
    return 0;
}

bool id_exists(const std::string& table, const std::string& id_column, int id) {
    sqlite3_stmt* stmt;
    std::string sql = "SELECT COUNT(*) FROM " + table + " WHERE " + id_column + " = ?;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count > 0;
}

json get_options(const std::string& table, const std::string& id_column, const std::string& name_column) {
    json result = json::array();

    sqlite3_stmt* stmt;
    std::string sql = "SELECT " + id_column + ", " + name_column + " FROM " + table + ";";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json option;
        option["id"] = sqlite3_column_int(stmt, 0);
        option["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        result.push_back(option);
    }
    sqlite3_finalize(stmt);

    return result;
}

std::vector<JobMatch> calculate_matches(const std::set<int>& user_skills, 
                                     const std::set<int>& user_interests, 
                                     const std::set<int>& user_traits) {
    std::vector<JobMatch> job_matches;

    std::set<int> enhanced_skills = user_skills;
    std::set<int> enhanced_traits = user_traits;

    for (int interest_id : user_interests) {
        sqlite3_stmt* stmt;
        const char* sql = "SELECT skill_id FROM interest_skills WHERE interest_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, interest_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            enhanced_skills.insert(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);

        sql = "SELECT personality_id FROM interest_personality WHERE interest_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, interest_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            enhanced_traits.insert(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    std::vector<int> job_ids;
    char* errorMsg = nullptr;
    sqlite3_exec(db, "SELECT job_id FROM jobs;", collect_single_id_callback, &job_ids, &errorMsg);
    if (errorMsg) sqlite3_free(errorMsg);

    for (int job_id : job_ids) {
        std::string job_title;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT job_title FROM jobs WHERE job_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, job_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            job_title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);

        std::vector<int> required_skills;
        sql = "SELECT skill_id FROM job_skills WHERE job_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, job_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            required_skills.push_back(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
        std::set<int> required_skills_set(required_skills.begin(), required_skills.end());

        std::vector<int> required_traits;
        sql = "SELECT personality_id FROM job_personality WHERE job_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, job_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            required_traits.push_back(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
        std::set<int> required_traits_set(required_traits.begin(), required_traits.end());

        std::set<int> skills_intersection;
        std::set_intersection(enhanced_skills.begin(), enhanced_skills.end(),
                            required_skills_set.begin(), required_skills_set.end(),
                            std::inserter(skills_intersection, skills_intersection.begin()));
        double skills_score = required_skills_set.empty() ? 1.0 : (double)skills_intersection.size() / required_skills_set.size();

        std::set<int> traits_intersection;
        std::set_intersection(enhanced_traits.begin(), enhanced_traits.end(),
                            required_traits_set.begin(), required_traits_set.end(),
                            std::inserter(traits_intersection, traits_intersection.begin()));
        double traits_score = required_traits_set.empty() ? 1.0 : (double)traits_intersection.size() / required_traits_set.size();

        double interest_score = skills_score;
        double total_score = (0.5 * skills_score) + (0.3 * traits_score) + (0.2 * interest_score);

        std::set<int> missing_skills;
        bool has_required_skills = !required_skills_set.empty();
        if (has_required_skills) {
            std::set_difference(required_skills_set.begin(), required_skills_set.end(),
                               enhanced_skills.begin(), enhanced_skills.end(),
                               std::inserter(missing_skills, missing_skills.begin()));
        }

        JobMatch match;
        match.job_id = job_id;
        match.job_title = job_title;
        match.score = total_score * 100;
        match.missing_skills = missing_skills;
        match.has_required_skills = has_required_skills;
        job_matches.push_back(match);
    }

    std::sort(job_matches.begin(), job_matches.end(),
              [](const JobMatch& a, const JobMatch& b) { return a.score > b.score; });

    return job_matches;
}

json get_skill_suggestions(const std::set<int>& user_skills, 
                         const std::set<int>& user_interests,
                         const std::vector<JobMatch>& job_matches) {
    json suggestions = json::array();

    std::set<int> all_missing_skills;
    for (const auto& match : job_matches) {
        all_missing_skills.insert(match.missing_skills.begin(), match.missing_skills.end());
    }

    std::set<int> interest_skills;
    for (int interest_id : user_interests) {
        sqlite3_stmt* stmt;
        const char* sql = "SELECT skill_id FROM interest_skills WHERE interest_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, interest_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int skill_id = sqlite3_column_int(stmt, 0);
            if (user_skills.find(skill_id) == user_skills.end()) {
                interest_skills.insert(skill_id);
            }
        }
        sqlite3_finalize(stmt);
    }

    std::set<int> all_suggestions;
    all_suggestions.insert(all_missing_skills.begin(), all_missing_skills.end());
    all_suggestions.insert(interest_skills.begin(), interest_skills.end());

    for (int skill_id : all_suggestions) {
        std::string skill_name;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT skill_name FROM skills WHERE skill_id = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) continue;

        sqlite3_bind_int(stmt, 1, skill_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            skill_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);

        if (!skill_name.empty()) {
            json suggestion;
            suggestion["id"] = skill_id;
            suggestion["name"] = skill_name;
            suggestion["type"] = (all_missing_skills.find(skill_id) != all_missing_skills.end() 
                                ? "missing_from_jobs" : "related_to_interests");
            suggestions.push_back(suggestion);
        }
    }

    return suggestions;
}

int main() {
    int rc = sqlite3_open("base.db", &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    Server svr;
    svr.set_mount_point("/", "./www");

    svr.Get("/api/options", [](const Request& req, Response& res) {
        json response;
        response["skills"] = get_options("skills", "skill_id", "skill_name");
        response["interests"] = get_options("interests", "interest_id", "interest_name");
        response["traits"] = get_options("personality_traits", "personality_id", "personality_name");
        res.set_content(response.dump(), "application/json");
    });

    svr.Post("/api/matches", [](const Request& req, Response& res) {
        json request = json::parse(req.body);

        std::set<int> user_skills, user_interests, user_traits;

        for (auto& skill : request["skills"]) {
            user_skills.insert(skill.get<int>());
        }
        for (auto& interest : request["interests"]) {
            user_interests.insert(interest.get<int>());
        }
        for (auto& trait : request["traits"]) {
            user_traits.insert(trait.get<int>());
        }

        auto job_matches = calculate_matches(user_skills, user_interests, user_traits);

        json response;
        json top_matches = json::array();

        for (size_t i = 0; i < std::min(job_matches.size(), size_t(3)); i++) {
            const auto& match = job_matches[i];
            json job;
            job["id"] = match.job_id;
            job["title"] = match.job_title;
            job["score"] = match.score;

            json missing_skills = json::array();
            for (int skill_id : match.missing_skills) {
                std::string skill_name;
                sqlite3_stmt* stmt;
                const char* sql = "SELECT skill_name FROM skills WHERE skill_id = ?;";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, skill_id);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        skill_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    }
                    sqlite3_finalize(stmt);
                }

                json skill;
                skill["id"] = skill_id;
                skill["name"] = skill_name.empty() ? "Unknown Skill" : skill_name;
                missing_skills.push_back(skill);
            }

            job["missing_skills"] = missing_skills;
            job["has_required_skills"] = match.has_required_skills;
            top_matches.push_back(job);
        }

        response["top_matches"] = top_matches;
        response["skill_suggestions"] = get_skill_suggestions(user_skills, user_interests, job_matches);
        res.set_content(response.dump(), "application/json");
    });

    svr.listen("0.0.0.0", 8080);
    return 0;
}
