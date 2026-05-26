// WorkboardDB — SQLite backing for the workboard task tracker.

#include "WorkboardDB.h"

#include <Urho3D/Container/HashSet.h>
#include <Urho3D/Core/StringUtils.h>
#include <Urho3D/IO/Compression.h>
#include <Urho3D/IO/Log.h>

#include <SQLite/sqlite3.h>

#include <cstdio>
#include <cctype>

WorkboardDB::WorkboardDB() = default;

WorkboardDB::~WorkboardDB()
{
    Close();
}

bool WorkboardDB::Open(const String& dbPath, const String& schemaPath)
{
    if (db_)
        Close();

    int rc = sqlite3_open(dbPath.CString(), &db_);
    if (rc != SQLITE_OK)
    {
        URHO3D_LOGERROR("WorkboardDB: Failed to open " + dbPath + ": " + String(sqlite3_errmsg(db_)));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // WAL mode for concurrent reads from shell commands
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    // Apply schema if provided
    if (!schemaPath.Empty())
    {
        FILE* f = fopen(schemaPath.CString(), "r");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* buf = new char[sz + 1];
            size_t read = fread(buf, 1, sz, f);
            buf[read] = '\0';
            fclose(f);

            char* errMsg = nullptr;
            rc = sqlite3_exec(db_, buf, nullptr, nullptr, &errMsg);
            delete[] buf;

            if (rc != SQLITE_OK)
            {
                String err = errMsg ? String(errMsg) : "unknown error";
                sqlite3_free(errMsg);
                URHO3D_LOGERROR("WorkboardDB: Schema error: " + err);
                // Non-fatal — schema may already exist
            }
        }
    }

    // Create memories table if not exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS memories ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  task     TEXT NOT NULL,"
        "  content  TEXT NOT NULL,"
        "  hash     TEXT UNIQUE,"
        "  created  INTEGER DEFAULT (strftime('%s','now'))"
        ");", nullptr, nullptr, nullptr);

    URHO3D_LOGINFO("WorkboardDB: Opened " + dbPath);
    return true;
}

void WorkboardDB::Close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ============================================================================
// Section name mappings
// ============================================================================

String WorkboardDB::SectionToDBName(const String& title)
{
    String t = title.ToLower().Trimmed();
    if (t.Contains("planned")) return "planned";
    if (t.Contains("open")) return "open";
    if (t.Contains("in progress")) return "in_progress";
    if (t.Contains("done")) return "done";
    return "";
}

String WorkboardDB::DBNameToTitle(const String& dbName)
{
    if (dbName == "planned") return "Planned";
    if (dbName == "open") return "Open";
    if (dbName == "in_progress") return "In Progress";
    if (dbName == "done") return "Done";
    return dbName;
}

Vector<String> WorkboardDB::GetColumnMapping(const String& dbSection)
{
    Vector<String> cols;
    if (dbSection == "planned")
    {
        cols.Push("priority"); cols.Push("task_name"); cols.Push("summary");
    }
    else if (dbSection == "open")
    {
        cols.Push("priority"); cols.Push("task_name"); cols.Push("blocked_by"); cols.Push("summary");
    }
    else if (dbSection == "in_progress")
    {
        cols.Push("task_name"); cols.Push("blocked_by"); cols.Push("summary");
    }
    else if (dbSection == "done")
    {
        cols.Push("task_name"); cols.Push("outcome"); cols.Push("learned"); cols.Push("summary");
    }
    return cols;
}

// ============================================================================
// Bootstrap
// ============================================================================

void WorkboardDB::BootstrapFromSections(const Vector<WorkboardSection>& sections)
{
    if (!db_) return;

    sqlite3_exec(db_, "DELETE FROM tasks;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO tasks "
        "(section, task_name, priority, outcome, learned, blocked_by, summary, sort_order) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);

    if (!stmt)
    {
        URHO3D_LOGERROR("WorkboardDB: Failed to prepare bootstrap insert");
        return;
    }

    int total = 0;
    for (unsigned s = 0; s < sections.Size(); ++s)
    {
        const WorkboardSection& sec = sections[s];
        String dbSection = SectionToDBName(sec.title);
        if (dbSection.Empty()) continue;

        Vector<String> colMap = GetColumnMapping(dbSection);

        for (unsigned r = 0; r < sec.rows.Size(); ++r)
        {
            const WorkboardRow& row = sec.rows[r];

            String taskName, outcome, learned, blockedBy, summary;
            int priority = 0;

            for (unsigned c = 0; c < colMap.Size() && c < row.cells.Size(); ++c)
            {
                String val = row.cells[c].Trimmed();
                val.Replace("**", "");
                if (val.StartsWith("`") && val.EndsWith("`"))
                    val = val.Substring(1, val.Length() - 2);

                const String& col = colMap[c];
                if (col == "task_name") taskName = val;
                else if (col == "priority") priority = val.Empty() ? 0 : atoi(val.CString());
                else if (col == "outcome") outcome = val;
                else if (col == "learned") learned = val;
                else if (col == "blocked_by") blockedBy = val;
                else if (col == "summary") summary = val;
            }

            if (taskName.Empty()) continue;

            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, dbSection.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, taskName.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, priority);
            sqlite3_bind_text(stmt, 4, outcome.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, learned.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, blockedBy.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, summary.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 8, (int)r);

            if (sqlite3_step(stmt) != SQLITE_DONE)
                URHO3D_LOGWARNING("WorkboardDB: Bootstrap insert failed for: " + taskName);
            else
                ++total;
        }
    }

    sqlite3_finalize(stmt);
    URHO3D_LOGINFO("WorkboardDB: Bootstrapped " + String(total) + " tasks");
}

void WorkboardDB::SyncFromSections(const Vector<WorkboardSection>& sections)
{
    if (!db_) return;

    // Collect all task names from in-memory sections
    HashSet<String> memoryTasks;

    sqlite3_stmt* upsert = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO tasks "
        "(section, task_name, priority, outcome, learned, blocked_by, summary, sort_order, "
        "created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, "
        "COALESCE((SELECT created_at FROM tasks WHERE task_name=? AND section=?), datetime('now')), "
        "datetime('now'))",
        -1, &upsert, nullptr);

    if (!upsert)
    {
        URHO3D_LOGERROR("WorkboardDB: Failed to prepare sync upsert");
        return;
    }

    for (unsigned s = 0; s < sections.Size(); ++s)
    {
        const WorkboardSection& sec = sections[s];
        String dbSection = SectionToDBName(sec.title);
        if (dbSection.Empty()) continue;

        Vector<String> colMap = GetColumnMapping(dbSection);

        for (unsigned r = 0; r < sec.rows.Size(); ++r)
        {
            const WorkboardRow& row = sec.rows[r];

            String taskName, outcome, learned, blockedBy, summary;
            int priority = 0;

            for (unsigned c = 0; c < colMap.Size() && c < row.cells.Size(); ++c)
            {
                String val = row.cells[c].Trimmed();
                val.Replace("**", "");
                if (val.StartsWith("`") && val.EndsWith("`"))
                    val = val.Substring(1, val.Length() - 2);

                const String& col = colMap[c];
                if (col == "task_name") taskName = val;
                else if (col == "priority") priority = val.Empty() ? 0 : atoi(val.CString());
                else if (col == "outcome") outcome = val;
                else if (col == "learned") learned = val;
                else if (col == "blocked_by") blockedBy = val;
                else if (col == "summary") summary = val;
            }

            if (taskName.Empty()) continue;
            memoryTasks.Insert(taskName);

            sqlite3_reset(upsert);
            sqlite3_bind_text(upsert, 1, dbSection.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert, 2, taskName.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upsert, 3, priority);
            sqlite3_bind_text(upsert, 4, outcome.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert, 5, learned.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert, 6, blockedBy.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert, 7, summary.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upsert, 8, (int)r);
            // For COALESCE on created_at
            sqlite3_bind_text(upsert, 9, taskName.CString(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upsert, 10, dbSection.CString(), -1, SQLITE_TRANSIENT);

            sqlite3_step(upsert);
        }
    }
    sqlite3_finalize(upsert);

    // Remove DB rows that are no longer in memory (task was deleted via mutation)
    sqlite3_stmt* query = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id, task_name FROM tasks", -1, &query, nullptr);
    Vector<int> toDelete;
    while (query && sqlite3_step(query) == SQLITE_ROW)
    {
        String name = (const char*)sqlite3_column_text(query, 1);
        if (!memoryTasks.Contains(name))
            toDelete.Push(sqlite3_column_int(query, 0));
    }
    sqlite3_finalize(query);

    if (!toDelete.Empty())
    {
        sqlite3_stmt* del = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM tasks WHERE id=?", -1, &del, nullptr);
        for (unsigned i = 0; i < toDelete.Size(); ++i)
        {
            sqlite3_reset(del);
            sqlite3_bind_int(del, 1, toDelete[i]);
            sqlite3_step(del);
        }
        sqlite3_finalize(del);
    }
}

// ============================================================================
// Load all sections
// ============================================================================

Vector<WorkboardSection> WorkboardDB::LoadAllSections()
{
    Vector<WorkboardSection> result;
    if (!db_) return result;

    // Section order matches workboard layout
    static const char* sectionOrder[] = {
        "planned", "open", "in_progress", "done"
    };
    static const char* headerSets[][5] = {
        {"Pri", "Task", "Summary", nullptr, nullptr},                // planned
        {"Pri", "Task", "Blocked By", "Summary", nullptr},           // open
        {"Task", "Blocked By", "Summary", nullptr, nullptr},         // in_progress
        {"Task", "Outcome", "Learned", "Summary", nullptr},          // done
    };

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT task_name, priority, outcome, learned, blocked_by, summary "
        "FROM tasks WHERE section=? ORDER BY sort_order, id",
        -1, &stmt, nullptr);

    if (!stmt) return result;

    for (int s = 0; s < 4; ++s)
    {
        WorkboardSection sec;
        sec.title = DBNameToTitle(sectionOrder[s]);

        // Set headers
        for (int h = 0; headerSets[s][h]; ++h)
            sec.headers.Push(headerSets[s][h]);

        Vector<String> colMap = GetColumnMapping(sectionOrder[s]);

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, sectionOrder[s], -1, SQLITE_STATIC);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            String taskName = (const char*)sqlite3_column_text(stmt, 0);
            int priority = sqlite3_column_int(stmt, 1);
            String outcome = (const char*)sqlite3_column_text(stmt, 2);
            String learned = (const char*)sqlite3_column_text(stmt, 3);
            String blockedBy = (const char*)sqlite3_column_text(stmt, 4);
            String summary = (const char*)sqlite3_column_text(stmt, 5);

            WorkboardRow row;
            for (unsigned c = 0; c < colMap.Size(); ++c)
            {
                const String& col = colMap[c];
                if (col == "task_name") row.cells.Push(taskName);
                else if (col == "priority") row.cells.Push(String(priority));
                else if (col == "outcome") row.cells.Push(outcome);
                else if (col == "learned") row.cells.Push(learned);
                else if (col == "blocked_by") row.cells.Push(blockedBy);
                else if (col == "summary") row.cells.Push(summary);
            }

            sec.rows.Push(row);
        }

        result.Push(sec);
    }

    sqlite3_finalize(stmt);
    return result;
}

// ============================================================================
// Query helpers
// ============================================================================

bool WorkboardDB::TaskExistsInSection(const String& taskName, const String& section)
{
    if (!db_) return false;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT COUNT(*) FROM tasks WHERE task_name=? AND section=?",
        -1, &stmt, nullptr);
    if (!stmt) return false;

    sqlite3_bind_text(stmt, 1, taskName.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, section.CString(), -1, SQLITE_TRANSIENT);

    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        exists = sqlite3_column_int(stmt, 0) > 0;

    sqlite3_finalize(stmt);
    return exists;
}

// ============================================================================
// Mutations
// ============================================================================

void WorkboardDB::InsertTask(const String& section, const String& taskName, int priority,
                              const String& outcome, const String& learned,
                              const String& blockedBy, const String& summary)
{
    if (!db_) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR REPLACE INTO tasks "
        "(section, task_name, priority, outcome, learned, blocked_by, summary) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);
    if (!stmt) return;

    sqlite3_bind_text(stmt, 1, section.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, taskName.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, priority);
    sqlite3_bind_text(stmt, 4, outcome.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, learned.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, blockedBy.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, summary.CString(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        URHO3D_LOGWARNING("WorkboardDB: Insert failed for: " + taskName);

    sqlite3_finalize(stmt);
}

void WorkboardDB::MoveTask(const String& taskName, const String& newSection)
{
    if (!db_) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE tasks SET section=?, updated_at=datetime('now') WHERE task_name=?",
        -1, &stmt, nullptr);
    if (!stmt) return;

    sqlite3_bind_text(stmt, 1, newSection.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, taskName.CString(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void WorkboardDB::RemoveTask(const String& taskName)
{
    if (!db_) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "DELETE FROM tasks WHERE task_name=?",
        -1, &stmt, nullptr);
    if (!stmt) return;

    sqlite3_bind_text(stmt, 1, taskName.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ============================================================================
// Reconciliation
// ============================================================================

Vector<WorkboardDiscrepancy> WorkboardDB::Reconcile(const Vector<WorkboardSection>& mdSections)
{
    Vector<WorkboardDiscrepancy> discrepancies;
    if (!db_) return discrepancies;

    for (unsigned s = 0; s < mdSections.Size(); ++s)
    {
        const WorkboardSection& sec = mdSections[s];
        String dbSection = SectionToDBName(sec.title);
        if (dbSection.Empty()) continue;

        Vector<String> colMap = GetColumnMapping(dbSection);
        int nameCol = -1;
        for (unsigned c = 0; c < colMap.Size(); ++c)
        {
            if (colMap[c] == "task_name") { nameCol = (int)c; break; }
        }
        if (nameCol < 0) continue;

        // Collect markdown task names
        HashSet<String> mdNames;
        for (unsigned r = 0; r < sec.rows.Size(); ++r)
        {
            if ((unsigned)nameCol < sec.rows[r].cells.Size())
            {
                String name = sec.rows[r].cells[nameCol].Trimmed();
                name.Replace("**", "");
                if (!name.Empty())
                    mdNames.Insert(name);
            }
        }

        // Collect DB task names
        HashSet<String> dbNames;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT task_name FROM tasks WHERE section=?",
            -1, &stmt, nullptr);
        if (stmt)
        {
            sqlite3_bind_text(stmt, 1, dbSection.CString(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* name = (const char*)sqlite3_column_text(stmt, 0);
                if (name)
                    dbNames.Insert(String(name));
            }
            sqlite3_finalize(stmt);
        }

        // Find discrepancies
        for (HashSet<String>::Iterator it = mdNames.Begin(); it != mdNames.End(); ++it)
        {
            if (!dbNames.Contains(*it))
            {
                WorkboardDiscrepancy d;
                d.section = dbSection;
                d.taskName = *it;
                d.type = WorkboardDiscrepancy::MARKDOWN_ONLY;
                discrepancies.Push(d);
            }
        }
        for (HashSet<String>::Iterator it = dbNames.Begin(); it != dbNames.End(); ++it)
        {
            if (!mdNames.Contains(*it))
            {
                WorkboardDiscrepancy d;
                d.section = dbSection;
                d.taskName = *it;
                d.type = WorkboardDiscrepancy::DB_ONLY;
                discrepancies.Push(d);
            }
        }
    }

    return discrepancies;
}

// ============================================================================
// Rules Engine
// ============================================================================

Vector<Rule> WorkboardDB::GetHookRules()
{
    Vector<Rule> rules;
    if (!db_) return rules;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, category, name, description, enforcement, pattern, action, message "
        "FROM rules WHERE active=1 AND enforcement='hook' AND pattern IS NOT NULL",
        -1, &stmt, nullptr);
    if (!stmt) return rules;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Rule r;
        r.id = sqlite3_column_int(stmt, 0);
        r.category = (const char*)sqlite3_column_text(stmt, 1);
        r.name = (const char*)sqlite3_column_text(stmt, 2);
        r.description = (const char*)sqlite3_column_text(stmt, 3);
        r.enforcement = (const char*)sqlite3_column_text(stmt, 4);
        r.pattern = sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
        r.action = (const char*)sqlite3_column_text(stmt, 6);
        r.message = sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
        r.active = true;
        rules.Push(r);
    }
    sqlite3_finalize(stmt);
    return rules;
}

Vector<Rule> WorkboardDB::GetAllRules()
{
    Vector<Rule> rules;
    if (!db_) return rules;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT id, category, name, description, enforcement, pattern, action, message, active "
        "FROM rules ORDER BY category, name",
        -1, &stmt, nullptr);
    if (!stmt) return rules;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Rule r;
        r.id = sqlite3_column_int(stmt, 0);
        r.category = (const char*)sqlite3_column_text(stmt, 1);
        r.name = (const char*)sqlite3_column_text(stmt, 2);
        r.description = (const char*)sqlite3_column_text(stmt, 3);
        r.enforcement = (const char*)sqlite3_column_text(stmt, 4);
        r.pattern = sqlite3_column_text(stmt, 5) ? (const char*)sqlite3_column_text(stmt, 5) : "";
        r.action = (const char*)sqlite3_column_text(stmt, 6);
        r.message = sqlite3_column_text(stmt, 7) ? (const char*)sqlite3_column_text(stmt, 7) : "";
        r.active = sqlite3_column_int(stmt, 8) != 0;
        rules.Push(r);
    }
    sqlite3_finalize(stmt);
    return rules;
}

Rule WorkboardDB::CheckCommand(const String& command)
{
    // Cache hook rules on first call
    static Vector<Rule> cachedRules;
    static bool cached = false;
    if (!cached)
    {
        cachedRules = GetHookRules();
        cached = true;
    }

    for (unsigned i = 0; i < cachedRules.Size(); ++i)
    {
        const Rule& r = cachedRules[i];
        if (r.pattern.Empty()) continue;

        // Simple substring match — patterns are human-readable identifiers
        // like "^make\b" or "git\s+push"
        // Use basic Contains check for non-regex patterns, regex for complex ones
        if (command.Contains(r.pattern) ||
            (r.pattern.StartsWith("^") && command.StartsWith(r.pattern.Substring(1))))
        {
            return r;
        }
    }

    return Rule{};  // empty = no match
}

void WorkboardDB::LogViolation(int ruleId, const String& role, const String& context, const String& outcome)
{
    if (!db_) return;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO violations (rule_id, role, context, outcome) VALUES (?, ?, ?, ?)",
        -1, &stmt, nullptr);
    if (!stmt) return;

    sqlite3_bind_int(stmt, 1, ruleId);
    sqlite3_bind_text(stmt, 2, role.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, context.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, outcome.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

Vector<Violation> WorkboardDB::GetRecentViolations(int limit)
{
    Vector<Violation> violations;
    if (!db_) return violations;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT v.id, r.name, v.role, v.timestamp, v.context, v.outcome "
        "FROM violations v JOIN rules r ON v.rule_id=r.id "
        "ORDER BY v.id DESC LIMIT ?",
        -1, &stmt, nullptr);
    if (!stmt) return violations;

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        Violation v;
        v.id = sqlite3_column_int(stmt, 0);
        v.ruleName = (const char*)sqlite3_column_text(stmt, 1);
        v.role = sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
        v.timestamp = (const char*)sqlite3_column_text(stmt, 3);
        v.context = sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "";
        v.outcome = (const char*)sqlite3_column_text(stmt, 5);
        violations.Push(v);
    }
    sqlite3_finalize(stmt);
    return violations;
}

int WorkboardDB::GetViolationCount()
{
    if (!db_) return 0;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM violations", -1, &stmt, nullptr);
    if (!stmt) return 0;

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// ── Shared memories ──

bool WorkboardDB::Remember(const String& task, const String& content)
{
    if (!db_ || task.Empty() || content.Empty())
        return false;

    // Simple hash: use content's StringHash for duplicate detection
    String hash = String(StringHash(content).Value());

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO memories (task, content, hash) VALUES (?, ?, ?);",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, task.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash.CString(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    bool inserted = (rc == SQLITE_DONE && sqlite3_changes(db_) > 0);
    if (inserted)
        URHO3D_LOGINFOF("[Memory] Stored: task='%s' content='%.60s...'", task.CString(), content.CString());
    return inserted;
}

Vector<String> WorkboardDB::GetMemoriesByTask(const String& task)
{
    Vector<String> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT content FROM memories WHERE task = ? ORDER BY created;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, task.CString(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        results.Push(String((const char*)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return results;
}

Vector<String> WorkboardDB::SearchMemories(const String& query)
{
    Vector<String> results;
    if (!db_ || query.Empty()) return results;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT task || ': ' || content FROM memories WHERE content LIKE '%' || ? || '%' ORDER BY created DESC LIMIT 50;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, query.CString(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        results.Push(String((const char*)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return results;
}

String WorkboardDB::GetAllMemories()
{
    String result;
    if (!db_) return result;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT task, content FROM memories ORDER BY created;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* task = (const char*)sqlite3_column_text(stmt, 0);
        const char* content = (const char*)sqlite3_column_text(stmt, 1);
        if (task && content)
        {
            result += task;
            result += ": ";
            result += content;
            result += "\n";
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

void WorkboardDB::PruneStaleMemories(unsigned maxAgeDays)
{
    if (!db_) return;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "DELETE FROM memories WHERE created < ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return;

    time_t cutoff = time(nullptr) - (time_t)(maxAgeDays * 86400);
    sqlite3_bind_int(stmt, 1, (int)cutoff);
    sqlite3_step(stmt);
    int pruned = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (pruned > 0)
        URHO3D_LOGINFOF("WorkboardDB: Pruned %d memories older than %u days", pruned, maxAgeDays);
}

int WorkboardDB::GetMemoryCount()
{
    if (!db_) return 0;
    int count = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM memories;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

// ── Death notes (compressed coder knowledge transfer) ──

int WorkboardDB::GetOrCreateToken(const String& term)
{
    if (!db_) return -1;

    // Try lookup first
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT token_id FROM dictionary WHERE term = ?;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, term.CString(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            int id = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            return id;
        }
    }
    sqlite3_finalize(stmt);

    // Insert new term
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO dictionary (term) VALUES (?);", -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, term.CString(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (int)sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return -1;
}

String WorkboardDB::LookupToken(int tokenId)
{
    if (!db_) return String::EMPTY;

    sqlite3_stmt* stmt = nullptr;
    String result;
    if (sqlite3_prepare_v2(db_, "SELECT term FROM dictionary WHERE token_id = ?;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(stmt, 1, tokenId);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            result = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
}

Vector<unsigned char> WorkboardDB::DictionaryEncode(const String& text)
{
    Vector<unsigned char> encoded;
    const char* p = text.CString();
    unsigned len = text.Length();
    unsigned i = 0;

    // Collect literal (non-word) characters into runs
    Vector<char> literalBuf;

    auto flushLiterals = [&]()
    {
        while (!literalBuf.Empty())
        {
            unsigned runLen = Min((unsigned)literalBuf.Size(), 127u);
            encoded.Push(0x80 | (unsigned char)runLen);
            for (unsigned j = 0; j < runLen; ++j)
                encoded.Push((unsigned char)literalBuf[j]);
            literalBuf.Erase(0, runLen);
        }
    };

    while (i < len)
    {
        // Word characters: letters, digits, underscore
        if (isalnum((unsigned char)p[i]) || p[i] == '_')
        {
            unsigned wordStart = i;
            while (i < len && (isalnum((unsigned char)p[i]) || p[i] == '_'))
                ++i;

            // Flush any pending literals before the word token
            flushLiterals();

            String word(p + wordStart, i - wordStart);
            int tokenId = GetOrCreateToken(word);

            // Write 2-byte token ID (big-endian, high bit clear)
            encoded.Push((unsigned char)((tokenId >> 8) & 0x7F));
            encoded.Push((unsigned char)(tokenId & 0xFF));
        }
        else
        {
            // Non-word character — accumulate as literal
            literalBuf.Push(p[i]);
            ++i;
        }
    }

    flushLiterals();
    return encoded;
}

String WorkboardDB::DictionaryDecode(const Vector<unsigned char>& encoded)
{
    String result;
    unsigned i = 0;
    unsigned size = encoded.Size();

    while (i < size)
    {
        if (encoded[i] & 0x80)
        {
            // Literal run
            unsigned runLen = encoded[i] & 0x7F;
            ++i;
            for (unsigned j = 0; j < runLen && i < size; ++j, ++i)
                result += (char)encoded[i];
        }
        else
        {
            // Token ID (2 bytes, big-endian)
            if (i + 1 >= size) break;
            int tokenId = ((int)encoded[i] << 8) | (int)encoded[i + 1];
            i += 2;
            result += LookupToken(tokenId);
        }
    }

    return result;
}

bool WorkboardDB::StoreDeath(const String& coder, const String& topic, const String& content)
{
    if (!db_ || content.Empty())
        return false;

    unsigned rawSize = content.Length();

    // Stage 1: dictionary encode
    Vector<unsigned char> dictEncoded = DictionaryEncode(content);
    unsigned dictSize = dictEncoded.Size();

    // Stage 2: LZ4 compress
    unsigned compBound = EstimateCompressBound(dictSize);

    // Build the blob: version(1) + dictSize(4) + rawSize(4) + compressed data
    unsigned headerSize = 9;
    Vector<unsigned char> blob(headerSize + compBound);

    blob[0] = 0x01;  // format version
    blob[1] = (unsigned char)((dictSize >> 24) & 0xFF);
    blob[2] = (unsigned char)((dictSize >> 16) & 0xFF);
    blob[3] = (unsigned char)((dictSize >> 8) & 0xFF);
    blob[4] = (unsigned char)(dictSize & 0xFF);
    blob[5] = (unsigned char)((rawSize >> 24) & 0xFF);
    blob[6] = (unsigned char)((rawSize >> 16) & 0xFF);
    blob[7] = (unsigned char)((rawSize >> 8) & 0xFF);
    blob[8] = (unsigned char)(rawSize & 0xFF);

    unsigned compSize = CompressData(&blob[headerSize], dictEncoded.Buffer(), dictSize);
    unsigned totalSize = headerSize + compSize;

    // Store in SQL
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO death_notes (coder, topic, compressed, raw_size) VALUES (?, ?, ?, ?);",
        -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, coder.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, topic.CString(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, blob.Buffer(), totalSize, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, rawSize);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE)
    {
        float ratio = rawSize > 0 ? (float)totalSize / (float)rawSize * 100.0f : 0.0f;
        URHO3D_LOGINFOF("StoreDeath: coder=%s topic=%s raw=%u compressed=%u (%.0f%%) dict=%d terms",
            coder.CString(), topic.CString(), rawSize, totalSize, ratio, GetDictionarySize());
        return true;
    }
    return false;
}

// (DecompressBlob is inlined in each caller to avoid access issues with private DictionaryDecode)

Vector<String> WorkboardDB::GetDeathNotes(const String& coder)
{
    Vector<String> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = coder.Empty()
        ? "SELECT compressed FROM death_notes ORDER BY created;"
        : "SELECT compressed FROM death_notes WHERE coder = ? ORDER BY created;";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    if (!coder.Empty())
        sqlite3_bind_text(stmt, 1, coder.CString(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char* blob = (const unsigned char*)sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize >= 9 && blob[0] == 0x01)
        {
            unsigned dictSize = ((unsigned)blob[1] << 24) | ((unsigned)blob[2] << 16) |
                                ((unsigned)blob[3] << 8) | (unsigned)blob[4];
            Vector<unsigned char> dictEncoded(dictSize);
            DecompressData(dictEncoded.Buffer(), blob + 9, dictSize);
            String text = DictionaryDecode(dictEncoded);
            if (!text.Empty())
                results.Push(text);
        }
    }
    sqlite3_finalize(stmt);
    return results;
}

Vector<String> WorkboardDB::GetDeathNotesByTopic(const String& topic)
{
    Vector<String> results;
    if (!db_ || topic.Empty()) return results;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT compressed FROM death_notes WHERE topic = ? ORDER BY created;",
        -1, &stmt, nullptr) != SQLITE_OK)
        return results;

    sqlite3_bind_text(stmt, 1, topic.CString(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char* blob = (const unsigned char*)sqlite3_column_blob(stmt, 0);
        int blobSize = sqlite3_column_bytes(stmt, 0);
        if (blob && blobSize >= 9 && blob[0] == 0x01)
        {
            unsigned dictSize = ((unsigned)blob[1] << 24) | ((unsigned)blob[2] << 16) |
                                ((unsigned)blob[3] << 8) | (unsigned)blob[4];
            Vector<unsigned char> dictEncoded(dictSize);
            DecompressData(dictEncoded.Buffer(), blob + 9, dictSize);
            String text = DictionaryDecode(dictEncoded);
            if (!text.Empty())
                results.Push(text);
        }
    }
    sqlite3_finalize(stmt);
    return results;
}

String WorkboardDB::GetTribalKnowledge()
{
    if (!db_) return String::EMPTY;

    String result;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT coder, topic, compressed, created FROM death_notes ORDER BY created;",
        -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* coder = (const char*)sqlite3_column_text(stmt, 0);
        const char* topic = (const char*)sqlite3_column_text(stmt, 1);
        const unsigned char* blob = (const unsigned char*)sqlite3_column_blob(stmt, 2);
        int blobSize = sqlite3_column_bytes(stmt, 2);
        const char* created = (const char*)sqlite3_column_text(stmt, 3);

        if (blob && blobSize >= 9 && blob[0] == 0x01)
        {
            unsigned dictSize = ((unsigned)blob[1] << 24) | ((unsigned)blob[2] << 16) |
                                ((unsigned)blob[3] << 8) | (unsigned)blob[4];
            Vector<unsigned char> dictEncoded(dictSize);
            DecompressData(dictEncoded.Buffer(), blob + 9, dictSize);
            String text = DictionaryDecode(dictEncoded);
            if (!text.Empty())
            {
                result += "--- ";
                result += coder ? coder : "?";
                if (topic && topic[0])
                {
                    result += " [";
                    result += topic;
                    result += "]";
                }
                result += " (";
                result += created ? created : "?";
                result += ") ---\n";
                result += text;
                result += "\n\n";
            }
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

int WorkboardDB::GetDictionarySize()
{
    if (!db_) return 0;
    int count = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM dictionary;", -1, &stmt, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}
