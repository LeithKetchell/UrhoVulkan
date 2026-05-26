// WorkboardDB — SQLite sole authority for the workboard task tracker.
// Follows GameDB pattern: raw sqlite3* handle, WAL mode, prepared statements.

#pragma once

#include "WorkboardProtocol.h"

#include <Urho3D/Container/HashMap.h>
#include <Urho3D/Container/Str.h>
#include <Urho3D/Container/Vector.h>

struct sqlite3;

using namespace Urho3D;

/// A single task row from the SQL database.
struct WorkboardTask
{
    int id{};
    String section;
    String taskName;
    int priority{};
    String outcome;
    String learned;
    String blockedBy;
    String summary;
    int sortOrder{};
};

/// A rule from the rules table.
struct Rule
{
    int id{};
    String category;
    String name;
    String description;
    String enforcement;   // hook, manager, advisory
    String pattern;       // regex
    String action;        // block, warn, log
    String message;
    bool active{true};
};

/// A logged violation.
struct Violation
{
    int id{};
    String ruleName;
    String role;
    String timestamp;
    String context;
    String outcome;
};

/// Discrepancy between markdown and DB.
struct WorkboardDiscrepancy
{
    String section;
    String taskName;
    enum Type { MARKDOWN_ONLY, DB_ONLY };
    Type type;
};

class WorkboardDB
{
public:
    WorkboardDB();
    ~WorkboardDB();

    /// Open database at dbPath, apply schema from schemaPath. Returns true on success.
    bool Open(const String& dbPath, const String& schemaPath);
    /// Close database.
    void Close();
    /// Return true if database is open.
    bool IsOpen() const { return db_ != nullptr; }

    /// Clear DB and re-import from parsed workboard sections (destructive — startup only).
    void BootstrapFromSections(const Vector<WorkboardSection>& sections);

    /// Non-destructive sync: upsert in-memory state, delete removed tasks only.
    void SyncFromSections(const Vector<WorkboardSection>& sections);

    /// Load all sections from DB into WorkboardSection format for UI.
    Vector<WorkboardSection> LoadAllSections();

    /// Check if a task exists in a specific section.
    bool TaskExistsInSection(const String& taskName, const String& section);

    /// Insert or replace a task.
    void InsertTask(const String& section, const String& taskName, int priority,
                    const String& outcome, const String& learned,
                    const String& blockedBy, const String& summary);
    /// Move a task to a new section.
    void MoveTask(const String& taskName, const String& newSection);
    /// Remove a task by name.
    void RemoveTask(const String& taskName);

    /// Compare DB against parsed markdown sections, return discrepancies.
    Vector<WorkboardDiscrepancy> Reconcile(const Vector<WorkboardSection>& mdSections);

    // ── Rules engine ──

    /// Get all active rules with enforcement type 'hook'.
    Vector<Rule> GetHookRules();
    /// Get all active rules.
    Vector<Rule> GetAllRules();
    /// Check a command against all active hook rules. Returns matching rule or empty.
    Rule CheckCommand(const String& command);
    /// Log a violation.
    void LogViolation(int ruleId, const String& role, const String& context, const String& outcome);
    /// Get recent violations.
    Vector<Violation> GetRecentViolations(int limit = 20);
    /// Get total violation count.
    int GetViolationCount();

    // ── Shared memories ──

    /// Store a memory tagged to a task. Duplicate content (same hash) is silently skipped.
    bool Remember(const String& task, const String& content);
    /// Search memories by task name.
    Vector<String> GetMemoriesByTask(const String& task);
    /// Search memories by content substring.
    Vector<String> SearchMemories(const String& query);
    /// Get all memories as a formatted text block.
    String GetAllMemories();
    /// Delete memories older than maxAgeDays.
    void PruneStaleMemories(unsigned maxAgeDays = 7);
    /// Get total memory count.
    int GetMemoryCount();

    // ── Death notes (compressed coder knowledge transfer) ──

    /// Store a death note — dictionary-encodes and LZ4-compresses the content.
    bool StoreDeath(const String& coder, const String& topic, const String& content);
    /// Retrieve all death notes for a coder (or all coders if empty), decompressed.
    Vector<String> GetDeathNotes(const String& coder = String::EMPTY);
    /// Retrieve death notes by topic, decompressed.
    Vector<String> GetDeathNotesByTopic(const String& topic);
    /// Get combined knowledge dump for a new spawn (all notes, all coders).
    String GetTribalKnowledge();
    /// Get dictionary size (diagnostic).
    int GetDictionarySize();

private:
    /// Look up or insert a term in the dictionary. Returns token_id.
    int GetOrCreateToken(const String& term);
    /// Look up a token_id in the dictionary. Returns the term or empty.
    String LookupToken(int tokenId);
    /// Dictionary-encode plaintext into a byte buffer.
    Vector<unsigned char> DictionaryEncode(const String& text);
    /// Dictionary-decode a byte buffer back to plaintext.
    String DictionaryDecode(const Vector<unsigned char>& encoded);
    /// Map section title from markdown to DB section name.
    static String SectionToDBName(const String& title);
    /// Map DB section name to display title.
    static String DBNameToTitle(const String& dbName);
    /// Get column mapping for a section (which header maps to which DB field).
    static Vector<String> GetColumnMapping(const String& dbSection);

    sqlite3* db_{nullptr};
};
