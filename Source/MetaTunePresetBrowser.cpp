#include "MetaTunePresetBrowser.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
    const juce::Colour browserPanel(0xff202024);
    const juce::Colour browserPanelInner(0xff17171b);
    const juce::Colour browserAccent(0xffa36bff);
    const juce::Colour browserCyan(0xff66dfff);
    const juce::Colour browserText(0xffeeeaf4);
    const juce::Colour browserMuted(0xffa7a3ae);

    juce::Font browserFont(float height, int style = juce::Font::plain)
    {
        auto options = juce::FontOptions("Segoe UI", height, style)
            .withMetricsKind(juce::TypefaceMetricsKind::portable)
            .withFallbacks(std::vector<juce::String>{ "Arial", "Tahoma" });
        return juce::Font(options);
    }

    void styleBrowserButton(juce::TextButton& button, bool primary = false)
    {
        button.setColour(juce::TextButton::buttonColourId,
                         primary ? juce::Colour(0xff4a326f) : juce::Colour(0xff29282e));
        button.setColour(juce::TextButton::buttonOnColourId,
                         primary ? juce::Colour(0xff68489a) : juce::Colour(0xff38353f));
        button.setColour(juce::TextButton::textColourOffId, browserText);
        button.setColour(juce::TextButton::textColourOnId, browserText);
        button.setClickingTogglesState(false);
    }
}

MetaTunePresetBrowser::MetaTunePresetBrowser(
    MetaTuneInspiredAudioProcessor& p,
    PresetLoadedCallback loadedCallback,
    juce::File requestedLibraryDirectory)
    : processor(p),
      onPresetLoaded(std::move(loadedCallback)),
      libraryDirectory(requestedLibraryDirectory == juce::File{}
                           ? getDefaultLibraryDirectory()
                           : requestedLibraryDirectory)
{
    libraryDirectory.createDirectory();
    getUserPresetDirectory(libraryDirectory).createDirectory();
    favoritesFile = libraryDirectory.getChildFile("Favorites.txt");

    setOpaque(false);
    setWantsKeyboardFocus(true);

    searchEditor.setMultiLine(false);
    searchEditor.setReturnKeyStartsNewLine(false);
    searchEditor.setTextToShowWhenEmpty("Search presets by name or folder...",
                                        browserMuted.withAlpha(0.78f));
    searchEditor.setColour(juce::TextEditor::backgroundColourId, browserPanelInner);
    searchEditor.setColour(juce::TextEditor::textColourId, browserText);
    searchEditor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff4a4851));
    searchEditor.setColour(juce::TextEditor::focusedOutlineColourId, browserAccent);
    searchEditor.setColour(juce::TextEditor::highlightColourId, browserAccent.withAlpha(0.35f));
    searchEditor.onTextChange = [this] { rebuildVisiblePresets(); };
    searchEditor.onReturnKey = [this]
    {
        if (presetList.getSelectedRow() >= 0)
            loadPresetAtRow(presetList.getSelectedRow());
    };
    searchEditor.onEscapeKey = [this] { setVisible(false); };
    addAndMakeVisible(searchEditor);

    categoryFilter.addItem("All presets", 1);
    categoryFilter.addItem("Favorites", 2);
    categoryFilter.addItem("User presets", 3);
    categoryFilter.setSelectedId(1, juce::dontSendNotification);
    categoryFilter.setColour(juce::ComboBox::backgroundColourId, browserPanelInner);
    categoryFilter.setColour(juce::ComboBox::textColourId, browserText);
    categoryFilter.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff4a4851));
    categoryFilter.setColour(juce::ComboBox::arrowColourId, browserAccent);
    categoryFilter.onChange = [this] { rebuildVisiblePresets(); };
    addAndMakeVisible(categoryFilter);

    presetList.setModel(this);
    presetList.setMultipleSelectionEnabled(false);
    presetList.setColour(juce::ListBox::backgroundColourId, browserPanelInner);
    presetList.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff3a3841));
    presetList.setOutlineThickness(1);
    presetList.setRowHeight(36);
    addAndMakeVisible(presetList);

    resultCount.setJustificationType(juce::Justification::centredRight);
    resultCount.setColour(juce::Label::textColourId, browserMuted);
    resultCount.setFont(browserFont(12.0f));
    addAndMakeVisible(resultCount);

    styleBrowserButton(loadButton, true);
    styleBrowserButton(saveButton);
    styleBrowserButton(importButton);
    styleBrowserButton(folderButton);
    styleBrowserButton(closeButton);
    loadButton.setEnabled(false);
    loadButton.onClick = [this]
    {
        const int selected = presetList.getSelectedRow();
        if (selected >= 0)
            loadPresetAtRow(selected);
    };
    saveButton.onClick = [this] { saveCurrentPreset(); };
    importButton.onClick = [this] { importPreset(); };
    folderButton.onClick = [this] { openPresetFolder(); };
    closeButton.onClick = [this] { setVisible(false); };

    addAndMakeVisible(loadButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(importButton);
    addAndMakeVisible(folderButton);
    addAndMakeVisible(closeButton);

    refresh();
}

MetaTunePresetBrowser::~MetaTunePresetBrowser()
{
    presetList.setModel(nullptr);
}

juce::File MetaTunePresetBrowser::getDefaultLibraryDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("MetaTune Inspired Presets");
}

juce::File MetaTunePresetBrowser::getUserPresetDirectory(const juce::File& root)
{
    const auto base = root == juce::File{} ? getDefaultLibraryDirectory() : root;
    return base.getChildFile("User");
}

void MetaTunePresetBrowser::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff050507).withAlpha(0.84f));

    auto panel = getLocalBounds().toFloat().reduced(
        juce::jmax(18.0f, (float)getWidth() * 0.045f),
        juce::jmax(18.0f, (float)getHeight() * 0.055f));

    g.setColour(juce::Colour(0xff050507).withAlpha(0.32f));
    g.fillRoundedRectangle(panel.translated(0.0f, 7.0f), 17.0f);
    g.setGradientFill(juce::ColourGradient(browserPanel.brighter(0.10f),
                                           panel.getX(), panel.getY(),
                                           browserPanel, panel.getRight(),
                                           panel.getBottom(), false));
    g.fillRoundedRectangle(panel, 17.0f);
    g.setColour(juce::Colour(0xff55515f));
    g.drawRoundedRectangle(panel, 17.0f, 1.0f);

    g.setColour(browserAccent.withAlpha(0.90f));
    g.fillRoundedRectangle(panel.getX() + 1.0f, panel.getY() + 1.0f,
                           panel.getWidth() - 2.0f, 3.0f, 1.5f);

    const float scale = juce::jlimit(0.78f, 1.25f,
        (float)getHeight() / 790.0f);
    const float titleSize = 21.0f * scale;
    g.setColour(browserText);
    g.setFont(browserFont(titleSize, juce::Font::bold));
    g.drawText("PRESET LIBRARY",
               juce::Rectangle<float>(panel.getX() + 26.0f,
                                      panel.getY() + 18.0f,
                                      panel.getWidth() - 52.0f, 30.0f),
               juce::Justification::centredLeft, true);

    g.setColour(browserMuted);
    g.setFont(browserFont(11.5f * scale));
    g.drawText("Search, favorite and recall your local .mti presets",
               juce::Rectangle<float>(panel.getX() + 27.0f,
                                      panel.getY() + 48.0f,
                                      panel.getWidth() - 54.0f, 22.0f),
               juce::Justification::centredLeft, true);

    const auto folderLabel = juce::String("LIBRARY  /  DOCUMENTS  /  METATUNE INSPIRED PRESETS");
    g.setColour(browserMuted.withAlpha(0.72f));
    g.setFont(browserFont(9.5f * scale, juce::Font::bold));
    g.drawText(folderLabel,
               juce::Rectangle<float>(panel.getX() + 27.0f,
                                      panel.getBottom() - 22.0f,
                                      panel.getWidth() - 54.0f, 14.0f),
               juce::Justification::centredLeft, true);
}

void MetaTunePresetBrowser::resized()
{
    auto panel = getLocalBounds().reduced(
        juce::jmax(18, juce::roundToInt((float)getWidth() * 0.045f)),
        juce::jmax(18, juce::roundToInt((float)getHeight() * 0.055f)));

    const float scale = juce::jlimit(0.78f, 1.25f,
        (float)getHeight() / 790.0f);
    const int pad = juce::jmax(18, juce::roundToInt(26.0f * scale));
    const int innerX = panel.getX() + pad;
    const int innerW = panel.getWidth() - pad * 2;
    const int searchY = panel.getY() + juce::roundToInt(83.0f * scale);
    const int controlH = juce::jmax(28, juce::roundToInt(37.0f * scale));

    searchEditor.setFont(browserFont(juce::jmax(10.0f, 13.0f * scale)));
    searchEditor.setBounds(innerX, searchY, juce::roundToInt(innerW * 0.59f), controlH);

    const int filterX = searchEditor.getRight() + juce::jmax(9, pad / 3);
    const int filterW = juce::jmin(170, innerW - (filterX - innerX) - 130);
    categoryFilter.setBounds(filterX, searchY, juce::jmax(120, filterW), controlH);

    resultCount.setBounds(categoryFilter.getRight() + 8, searchY,
                          juce::jmax(70, panel.getRight() - pad - categoryFilter.getRight() - 8),
                          controlH);
    const int listY = searchY + controlH + juce::jmax(12, juce::roundToInt(13.0f * scale));
    const int footerH = juce::jmax(35, juce::roundToInt(43.0f * scale));
    const int footerY = panel.getBottom() - pad - footerH;
    presetList.setRowHeight(juce::jlimit(30, 44, juce::roundToInt(36.0f * scale)));
    presetList.setBounds(innerX, listY, innerW, juce::jmax(80, footerY - listY - 12));

    const int gap = juce::jmax(6, juce::roundToInt(8.0f * scale));
    const int footerWidth = juce::jmin(innerW,
        juce::jmax(520, juce::roundToInt(760.0f * scale)));
    const int buttonW = (footerWidth - gap * 4) / 5;
    int x = panel.getCentreX() - footerWidth / 2;
    juce::TextButton* buttons[] = { &loadButton, &saveButton, &importButton, &folderButton, &closeButton };
    for (int i = 0; i < 5; ++i)
    {
        // JUCE TextButton uses the active LookAndFeel font; it has no setFont API.
        buttons[i]->setBounds(x, footerY, buttonW, footerH);
        x += buttonW + gap;
    }
}

void MetaTunePresetBrowser::focusSearch()
{
    searchEditor.grabKeyboardFocus();
}

void MetaTunePresetBrowser::refresh()
{
    libraryDirectory.createDirectory();
    getUserPresetDirectory(libraryDirectory).createDirectory();
    loadFavorites();

    juce::Array<juce::File> found;
    libraryDirectory.findChildFiles(found, juce::File::findFiles, true, "*.mti");

    allPresets.clear();
    allPresets.reserve((size_t)found.size());
    for (int i = 0; i < found.size(); ++i)
    {
        const auto file = found.getReference(i);
        if (file.getFileExtension().equalsIgnoreCase(".mti"))
            allPresets.push_back(file);
    }

    std::sort(allPresets.begin(), allPresets.end(),
        [](const juce::File& a, const juce::File& b)
        {
            const int byName = a.getFileNameWithoutExtension()
                                .compareIgnoreCase(b.getFileNameWithoutExtension());
            if (byName != 0)
                return byName < 0;
            return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0;
        });

    rebuildVisiblePresets();
}

void MetaTunePresetBrowser::rebuildVisiblePresets()
{
    const auto query = searchEditor.getText().trim();
    const int filter = categoryFilter.getSelectedId();
    const auto userDirectory = getUserPresetDirectory(libraryDirectory);

    visiblePresets.clear();
    for (const auto& file : allPresets)
    {
        const bool favorite = isFavorite(file);
        if (filter == 2 && !favorite)
            continue;
        if (filter == 3 && !file.isAChildOf(userDirectory))
            continue;

        if (query.isNotEmpty())
        {
            const auto name = file.getFileNameWithoutExtension();
            const auto category = file.getParentDirectory().getFileName();
            const auto relative = relativePresetPath(file);
            if (!name.containsIgnoreCase(query)
                && !category.containsIgnoreCase(query)
                && !relative.containsIgnoreCase(query))
                continue;
        }
        visiblePresets.push_back(file);
    }

    presetList.updateContent();
    presetList.setRowHeight(juce::jlimit(30, 44,
        juce::roundToInt(36.0f * juce::jlimit(0.78f, 1.25f,
                                             (float)getHeight() / 790.0f))));
    if (visiblePresets.empty())
        presetList.deselectAllRows();
    else
        presetList.selectRow(0, juce::dontSendNotification);

    loadButton.setEnabled(!visiblePresets.empty());
    const auto countText = juce::String((int)visiblePresets.size())
                         + (visiblePresets.size() == 1 ? " preset" : " presets");
    resultCount.setText(countText, juce::dontSendNotification);
    resultCount.setColour(juce::Label::textColourId,
                          visiblePresets.empty() ? browserMuted : browserCyan);
    repaint();
}

int MetaTunePresetBrowser::getNumRows()
{
    return (int)visiblePresets.size();
}

int MetaTunePresetBrowser::getVisiblePresetCount() const noexcept
{
    return (int)visiblePresets.size();
}

void MetaTunePresetBrowser::paintListBoxItem(
    int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= (int)visiblePresets.size())
        return;

    const auto& file = visiblePresets[(size_t)rowNumber];
    if (rowIsSelected)
    {
        g.setColour(browserAccent.withAlpha(0.17f));
        g.fillRoundedRectangle(3.0f, 2.0f, (float)width - 6.0f,
                               (float)height - 4.0f, 6.0f);
        g.setColour(browserAccent.withAlpha(0.90f));
        g.fillRoundedRectangle(3.0f, 7.0f, 2.5f,
                               (float)height - 14.0f, 1.0f);
    }
    else
    {
        g.setColour(juce::Colour(0xff2f2c35).withAlpha(0.56f));
        g.drawLine(12.0f, (float)height - 1.0f,
                   (float)width - 12.0f, (float)height - 1.0f, 0.6f);
    }

    const float scale = juce::jlimit(0.78f, 1.25f,
        (float)getHeight() / 790.0f);
    const float titleH = juce::jmax(11.0f, 14.0f * scale);
    g.setColour(browserText);
    g.setFont(browserFont(titleH, juce::Font::bold));
    g.drawText(file.getFileNameWithoutExtension(),
               juce::Rectangle<float>(18.0f, 1.0f, (float)width - 132.0f,
                                      (float)height * 0.61f),
               juce::Justification::centredLeft, true);

    g.setColour(browserMuted);
    g.setFont(browserFont(juce::jmax(8.5f, 10.0f * scale)));
    const auto category = file.getParentDirectory().getFileName();
    g.drawText(category.isNotEmpty() ? category : "Library",
               juce::Rectangle<float>(19.0f, (float)height * 0.53f,
                                      (float)width - 132.0f,
                                      (float)height * 0.38f),
               juce::Justification::centredLeft, true);

    const float cx = (float)width - 20.0f;
    const float cy = (float)height * 0.5f;
    juce::Path star;
    for (int i = 0; i < 10; ++i)
    {
        const float angle = -juce::MathConstants<float>::halfPi
                          + (float)i * juce::MathConstants<float>::pi / 5.0f;
        const float radius = (i % 2 == 0) ? 8.5f : 3.8f;
        const juce::Point<float> point(cx + std::cos(angle) * radius,
                                       cy + std::sin(angle) * radius);
        if (i == 0) star.startNewSubPath(point);
        else star.lineTo(point);
    }
    star.closeSubPath();
    if (isFavorite(file))
    {
        g.setColour(browserAccent);
        g.fillPath(star);
    }
    else
    {
        g.setColour(browserMuted.withAlpha(0.75f));
        g.strokePath(star, juce::PathStrokeType(1.1f));
    }
}

void MetaTunePresetBrowser::selectedRowsChanged(int row)
{
    loadButton.setEnabled(row >= 0 && row < (int)visiblePresets.size());
}

void MetaTunePresetBrowser::listBoxItemClicked(int row, const juce::MouseEvent& event)
{
    if (row < 0 || row >= (int)visiblePresets.size())
        return;
    if (event.x >= presetList.getWidth() - 43)
    {
        toggleFavoriteAtRow(row);
        return;
    }
    presetList.selectRow(row, false);
}

void MetaTunePresetBrowser::listBoxItemDoubleClicked(
    int row, const juce::MouseEvent&)
{
    loadPresetAtRow(row);
}

void MetaTunePresetBrowser::setSearchQuery(const juce::String& query)
{
    searchEditor.setText(query, juce::dontSendNotification);
    rebuildVisiblePresets();
}

void MetaTunePresetBrowser::setCategoryFilter(int itemId)
{
    categoryFilter.setSelectedId(itemId, juce::dontSendNotification);
    rebuildVisiblePresets();
}

void MetaTunePresetBrowser::toggleFavoriteAtRow(int row)
{
    if (row < 0 || row >= (int)visiblePresets.size())
        return;

    const auto path = relativePresetPath(visiblePresets[(size_t)row]);
    const int existing = favoritePaths.indexOf(path);
    if (existing >= 0)
        favoritePaths.remove(existing);
    else
        favoritePaths.add(path);
    saveFavorites();
    rebuildVisiblePresets();
}

bool MetaTunePresetBrowser::isFavorite(const juce::File& file) const
{
    return favoritePaths.contains(relativePresetPath(file));
}

juce::String MetaTunePresetBrowser::relativePresetPath(const juce::File& file) const
{
    return file.getRelativePathFrom(libraryDirectory).replaceCharacter('\\', '/');
}

void MetaTunePresetBrowser::loadFavorites()
{
    favoritePaths.clear();
    if (!favoritesFile.existsAsFile())
        return;

    juce::StringArray lines;
    lines.addLines(favoritesFile.loadFileAsString());
    for (const auto& line : lines)
    {
        const auto clean = line.trim();
        if (clean.isNotEmpty() && !favoritePaths.contains(clean))
            favoritePaths.add(clean);
    }
}

void MetaTunePresetBrowser::saveFavorites()
{
    libraryDirectory.createDirectory();
    if (!favoritesFile.replaceWithText(favoritePaths.joinIntoString("\n")))
        showError("Favorites could not be saved",
                  "The favorites list could not be written to the preset library folder.");
}

bool MetaTunePresetBrowser::loadPresetAtRow(int row)
{
    if (row < 0 || row >= (int)visiblePresets.size())
        return false;

    const auto file = visiblePresets[(size_t)row];
    if (!file.existsAsFile() || file.getSize() <= 0
        || file.getSize() > 32 * 1024 * 1024)
    {
        showError("Preset could not be loaded",
                  "The selected .mti file is missing, empty, or larger than 32 MB.");
        return false;
    }

    juce::MemoryBlock state;
    if (!file.loadFileAsData(state) || state.getSize() == 0)
    {
        showError("Preset could not be loaded",
                  "The selected preset file could not be read.");
        return false;
    }

    std::unique_ptr<juce::XmlElement> xml(
        juce::AudioProcessor::getXmlFromBinary(state.getData(), (int)state.getSize()));
    if (xml == nullptr || !xml->hasTagName(processor.parameters.state.getType()))
    {
        showError("Invalid MetaTune preset",
                  "This file is not a valid MetaTune Inspired .mti preset.");
        return false;
    }

    processor.setStateInformation(state.getData(), (int)state.getSize());
    if (onPresetLoaded)
        onPresetLoaded(file);
    setVisible(false);
    return true;
}

void MetaTunePresetBrowser::selectFile(const juce::File& file)
{
    for (int i = 0; i < (int)visiblePresets.size(); ++i)
    {
        if (visiblePresets[(size_t)i] == file)
        {
            presetList.selectRow(i, false);
            return;
        }
    }
}

void MetaTunePresetBrowser::saveCurrentPreset()
{
    if (fileChooser != nullptr)
        return;

    const auto userDirectory = getUserPresetDirectory(libraryDirectory);
    userDirectory.createDirectory();
    const auto defaultFile = userDirectory.getChildFile("New Preset.mti");
    fileChooser = std::make_shared<juce::FileChooser>(
        "Save MetaTune Inspired preset", defaultFile, "*.mti");

    auto chooser = fileChooser;
    juce::Component::SafePointer<MetaTunePresetBrowser> safeThis(this);
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            auto file = fc.getResult();
            if (file != juce::File{})
            {
                if (!file.getFileExtension().equalsIgnoreCase(".mti"))
                    file = file.getParentDirectory().getChildFile(
                        file.getFileNameWithoutExtension() + ".mti");

                file.getParentDirectory().createDirectory();
                juce::MemoryBlock state;
                safeThis->processor.getStateInformation(state);
                if (state.isEmpty()
                    || !file.replaceWithData(state.getData(), state.getSize()))
                {
                    safeThis->showError("Preset save failed",
                        "The selected preset file could not be written.");
                }
                else
                {
                    safeThis->refresh();
                    safeThis->selectFile(file);
                    if (safeThis->onPresetLoaded)
                        safeThis->onPresetLoaded(file);
                }
            }
            safeThis->fileChooser.reset();
        });
}

void MetaTunePresetBrowser::importPreset()
{
    if (fileChooser != nullptr)
        return;

    fileChooser = std::make_shared<juce::FileChooser>(
        "Import MetaTune Inspired preset",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.mti");
    auto chooser = fileChooser;
    juce::Component::SafePointer<MetaTunePresetBrowser> safeThis(this);
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr)
                return;

            const auto source = fc.getResult();
            if (source != juce::File{})
            {
                if (!source.existsAsFile()
                    || !source.getFileExtension().equalsIgnoreCase(".mti"))
                {
                    safeThis->showError("Unsupported preset",
                        "Choose a MetaTune Inspired .mti preset file.");
                }
                else
                {
                    const auto userDirectory =
                        MetaTunePresetBrowser::getUserPresetDirectory(safeThis->libraryDirectory);
                    userDirectory.createDirectory();

                    auto destination = userDirectory.getChildFile(source.getFileName());
                    int suffix = 2;
                    while (destination.existsAsFile())
                    {
                        destination = userDirectory.getChildFile(
                            source.getFileNameWithoutExtension()
                            + " (" + juce::String(suffix++) + ").mti");
                    }

                    if (!source.copyFileTo(destination))
                    {
                        safeThis->showError("Preset import failed",
                            "The preset could not be copied into the local library.");
                    }
                    else
                    {
                        safeThis->refresh();
                        safeThis->selectFile(destination);
                    }
                }
            }
            safeThis->fileChooser.reset();
        });
}

void MetaTunePresetBrowser::openPresetFolder()
{
    libraryDirectory.createDirectory();
    libraryDirectory.startAsProcess();
}

void MetaTunePresetBrowser::showError(
    const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::WarningIcon, title, message);
}
