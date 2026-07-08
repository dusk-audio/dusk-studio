#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>

//==============================================================================
// Chord quality enumeration
enum class ChordQuality
{
    Major,
    Minor,
    Diminished,
    Augmented,
    Dominant7,
    Major7,
    Minor7,
    MinorMajor7,
    Diminished7,
    HalfDiminished7,
    Augmented7,
    AugmentedMajor7,
    Sus2,
    Sus4,
    Dominant7Sus4,
    Add9,
    Add11,
    Major6,
    Minor6,
    Major9,
    Minor9,
    Dominant9,
    Major11,
    Minor11,
    Dominant11,
    Major13,
    Minor13,
    Dominant13,
    Power5,
    Dominant7Flat5,
    Dominant7Sharp5,
    Dominant7Flat9,
    Dominant7Sharp9,
    Unknown
};

//==============================================================================
// Harmonic function enumeration
enum class HarmonicFunction
{
    Tonic,           // I, vi, iii
    Subdominant,     // IV, ii
    Dominant,        // V, vii
    SecondaryDom,    // V/x chords
    Borrowed,        // Modal interchange (bVII, bVI, etc.)
    Chromatic,       // Outside the key
    Unknown
};

//==============================================================================
// Suggestion category
enum class SuggestionCategory
{
    Basic,          // Common progressions (I-IV-V-I)
    Intermediate,   // Secondary dominants, borrowed chords
    Advanced        // Modal interchange, tritone subs, chromatic mediants
};

//==============================================================================
// Chord information structure
struct ChordInfo
{
    std::string name;              // e.g., "Cmaj7", "Dm", "G7"
    std::string romanNumeral;      // e.g., "I", "ii", "V7"
    HarmonicFunction function = HarmonicFunction::Unknown;
    std::vector<int> midiNotes;     // MIDI note numbers (sorted)
    int rootNote = -1;              // Root pitch class (0-11, C=0)
    int bassNote = -1;              // Lowest note pitch class
    ChordQuality quality = ChordQuality::Unknown;
    std::string extensions;        // Any additional text
    int inversion = 0;              // 0=root, 1=first, 2=second, etc.
    bool isValid = false;
    float confidence = 0.0f;        // 0.0-1.0 confidence score

    bool operator==(const ChordInfo& other) const
    {
        return name == other.name && rootNote == other.rootNote && quality == other.quality;
    }

    bool operator!=(const ChordInfo& other) const
    {
        return !(*this == other);
    }
};

//==============================================================================
// Chord suggestion structure
struct ChordSuggestion
{
    std::string romanNumeral;
    std::string chordName;         // Actual chord name in current key
    SuggestionCategory category;
    std::string reason;            // Why this suggestion makes sense
    float commonality = 0.5f;       // How common this progression is (0.0-1.0)
};

//==============================================================================
// Main chord analyzer class
class ChordAnalyzer
{
public:
    ChordAnalyzer();

    //==========================================================================
    // Main analysis function
    ChordInfo analyze(const std::vector<int>& midiNotes);

    //==========================================================================
    // Key context
    void setKey(int rootNote, bool isMinor);
    int getKeyRoot() const { return keyRoot; }
    bool isMinorKey() const { return minorKey; }
    std::string getKeyName() const;

    //==========================================================================
    // Get Roman numeral for chord in current key
    std::string getRomanNumeral(const ChordInfo& chord) const;

    // Get harmonic function
    HarmonicFunction getHarmonicFunction(int chordRoot, ChordQuality quality) const;

    //==========================================================================
    // Get chord suggestions based on current chord
    std::vector<ChordSuggestion> getSuggestions(const ChordInfo& currentChord,
                                                 SuggestionCategory maxLevel = SuggestionCategory::Advanced) const;

    //==========================================================================
    // Static utilities
    static std::string noteToName(int midiNote, bool useFlats = false);
    static std::string pitchClassToName(int pitchClass, bool useFlats = false);
    static int nameToNote(const std::string& name);
    static std::string qualityToString(ChordQuality quality);
    static std::string qualityToSuffix(ChordQuality quality);
    static std::string functionToString(HarmonicFunction func);

private:
    int keyRoot = 0;        // C
    bool minorKey = false;

    //==========================================================================
    // Interval pattern matching
    struct ChordPattern
    {
        std::set<int> intervals;    // Semitone intervals from root
        ChordQuality quality;
        std::string suffix;
        int priority;               // Higher = preferred match
    };

    static const std::vector<ChordPattern> chordPatterns;

    //==========================================================================
    // Analysis helpers
    int findRoot(const std::vector<int>& notes) const;
    std::set<int> getIntervals(const std::vector<int>& notes, int root) const;
    // Returns the matched pattern by index into chordPatterns, or -1 on no
    // match. Returning the index (rather than the ChordQuality) lets
    // calculateConfidence score against the exact pattern that matched -
    // distinct patterns can share a ChordQuality (alias spellings), so a
    // quality-only lookup picked the wrong pattern's interval set.
    int  matchPattern(const std::set<int>& intervals, int& outPriority) const;
    int calculateInversion(const std::vector<int>& notes, int root) const;
    float calculateConfidence(const std::set<int>& intervals, int matchedPatternIndex) const;

    //==========================================================================
    // Roman numeral helpers
    int getScaleDegree(int chordRoot) const;
    bool isChromatic(int chordRoot) const;
    std::string getAccidental(int chordRoot) const;
    std::string degreeToRoman(int degree, bool uppercase) const;
    std::string buildRomanNumeral(int chordRoot, ChordQuality quality) const;

    //==========================================================================
    // Suggestion generation
    std::string getRootNameInKey(int degree) const;
    std::string getSpellingForKey(int pitchClass) const;
    void addBasicSuggestions(std::vector<ChordSuggestion>& suggestions, int currentDegree, ChordQuality quality) const;
    void addIntermediateSuggestions(std::vector<ChordSuggestion>& suggestions, int currentDegree, ChordQuality quality) const;
    void addAdvancedSuggestions(std::vector<ChordSuggestion>& suggestions, int currentDegree, ChordQuality quality) const;

    ChordAnalyzer (const ChordAnalyzer&)            = delete;
    ChordAnalyzer& operator= (const ChordAnalyzer&) = delete;
};
