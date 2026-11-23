#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <functional>
#include <vector>

// A small, local-only preset library.  Presets remain the processor's native
// APVTS XML state (.mti); the browser never introduces new automation IDs.
class MetaTunePresetBrowser final : public juce::Component,
                                    private juce::ListBoxModel
{
public:
    using PresetLoadedCallback = std::function<void(const juce::File&)>;

    MetaTunePresetBrowser(MetaTuneInspiredAudioProcessor&,
                          PresetLoadedCallback,
                          juce::File libraryDirectory = {});
    ~MetaTunePresetBrowser() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh();
    void focusSearch();

    int getVisiblePresetCount() const noexcept;
    bool loadPresetAtRow(int row);
    void setSearchQuery(const juce::String&);
    void setCategoryFilter(int itemId);
    void toggleFavoriteAtRow(int row);

    static juce::File getDefaultLibraryDirectory();
    static juce::File getUserPresetDirectory(const juce::File& libraryDirectory = {});
    // Never replace an existing user preset, even when the save dialog chooses its name.
    static juce::File getAvailablePresetFile(const juce::File& directory,
                                              const juce::String& requestedBaseName);

private:
    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics&, int width, int height,
                          bool rowIsSelected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    void rebuildVisiblePresets();
    void loadFavorites();
    void saveFavorites();
    bool isValidPresetData(const juce::MemoryBlock&) const;
    bool isFavorite(const juce::File&) const;
    juce::String relativePresetPath(const juce::File&) const;
    void saveCurrentPreset();
    void importPreset();
    void openPresetFolder();
    void selectFile(const juce::File&);
    void showError(const juce::String& title, const juce::String& message);

    MetaTuneInspiredAudioProcessor& processor;
    PresetLoadedCallback onPresetLoaded;
    juce::File libraryDirectory;
    juce::File favoritesFile;
    std::vector<juce::File> allPresets;
    std::vector<juce::File> visiblePresets;
    juce::StringArray favoritePaths;

    juce::TextEditor searchEditor;
    juce::ComboBox categoryFilter;
    juce::ListBox presetList;
    juce::Label resultCount;
    juce::TextButton loadButton { "LOAD SELECTED" };
    juce::TextButton saveButton { "SAVE CURRENT" };
    juce::TextButton importButton { "IMPORT .MTI" };
    juce::TextButton folderButton { "OPEN FOLDER" };
    juce::TextButton closeButton { "CLOSE" };
    std::shared_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetaTunePresetBrowser)
};
