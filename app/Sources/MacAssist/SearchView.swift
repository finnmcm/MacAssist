import SwiftUI

// The panel's content: a search field over a ranked, actionable results
// list, with a "Show all N in Finder" footer. Keyboard navigation (arrows,
// Enter, ⌘Enter, Esc) is handled by a key monitor in PanelController so it
// works regardless of SwiftUI focus quirks; this view stays presentational.
//
// Every section has a fixed height drawn from PanelMetrics, so the view's
// total height matches the window height PanelController computes — that's
// what keeps the results from being clipped.
struct SearchView: View {
    @ObservedObject var model: SearchModel
    @FocusState private var fieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            searchField
            toolbar

            if !model.results.isEmpty {
                Divider()
                resultsList
                Divider()
                footer
            } else if !model.status.isEmpty {
                Divider()
                Text(model.status)
                    .foregroundStyle(.secondary)
                    .font(.system(size: 13))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 18)
                    .frame(height: PanelMetrics.statusHeight)
            }
        }
        .frame(width: PanelMetrics.width)
        .onAppear { fieldFocused = true }
    }

    private var searchField: some View {
        HStack(spacing: 10) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
                .font(.system(size: 17, weight: .medium))
            // Active filters render as chips to the left of the cursor.
            ForEach(model.activeFilters) { filter in
                FilterChip(filter: filter) { model.remove(filter) }
            }
            TextField("Search your files…", text: $model.queryText)
                .textFieldStyle(.plain)
                .font(.system(size: 20, weight: .regular))
                .focused($fieldFocused)
        }
        .padding(.horizontal, 18)
        .frame(height: PanelMetrics.fieldHeight)
    }

    // Always-visible row of broad filter toggles. Labels are display-only; the
    // canonical daemon kind travels in the Filter (see SearchModel.Filter).
    private var toolbar: some View {
        HStack(spacing: 8) {
            toolbarButton("Recently downloaded", .recentlyDownloaded)
            toolbarButton("Photos", .kind("image"))
            toolbarButton("Videos", .kind("video"))
            toolbarButton("Music", .kind("audio"))
            toolbarButton("Documents", .kind("document"))
            toolbarButton("Code", .kind("code"))
            tagsMenu
            Spacer()
        }
        .padding(.horizontal, 14)
        .frame(height: PanelMetrics.toolbarHeight)
    }

    private func toolbarButton(_ label: String, _ filter: Filter) -> some View {
        let active = model.isActive(filter)
        return Text(label)
            .font(.system(size: 12, weight: .medium))
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(
                Capsule().fill(active ? Color.accentColor
                                      : Color.secondary.opacity(0.15)))
            .foregroundStyle(active ? Color.white : Color.primary)
            .contentShape(Capsule())
            .onTapGesture { model.toggle(filter) }
    }

    // "Tags ▾": a menu of the 7 Finder colors; each pick toggles a tag chip.
    private var tagsMenu: some View {
        Menu {
            ForEach(1...7, id: \.self) { color in
                Button {
                    model.toggle(.tag(color: color))
                } label: {
                    Label(FilterLabels.colorName(color),
                          systemImage: model.isActive(.tag(color: color))
                              ? "checkmark" : "circle.fill")
                }
            }
        } label: {
            HStack(spacing: 3) {
                Text("Tags")
                Image(systemName: "chevron.down").font(.system(size: 8))
            }
            .font(.system(size: 12, weight: .medium))
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(Capsule().fill(Color.secondary.opacity(0.15)))
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
    }

    private var resultsList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 0) {
                    // Keyed by index only. Selection and scroll-to are
                    // index-based, and we replace the whole array on each
                    // query, so a single index identity keeps the rendered
                    // rows exactly in sync with `results` (mixing index and
                    // fileId identities made rows go stale / disappear).
                    ForEach(model.results.indices, id: \.self) { index in
                        ResultRow(hit: model.results[index],
                                  selected: index == model.selection)
                            .frame(height: PanelMetrics.rowHeight)
                            .id(index)
                            .contentShape(Rectangle())
                            // A click only highlights the row; revealing/opening
                            // stays on the explicit keys (Enter reveals, ⌘E
                            // opens in the editor) so a click never yanks focus
                            // to Finder.
                            .onTapGesture { model.selection = index }
                    }
                }
                .padding(.vertical, PanelMetrics.listVPadding / 2)
            }
            .frame(height: PanelMetrics.listHeight(rowCount: model.results.count))
            .onChange(of: model.selection) { _, newValue in
                withAnimation(.easeOut(duration: 0.12)) {
                    proxy.scrollTo(newValue, anchor: .center)
                }
            }
        }
    }

    private var footer: some View {
        HStack(spacing: 8) {
            Image(systemName: "folder")
            Text("Show all \(model.results.count) in Finder")
            Spacer()
            Text("⌘↵")
        }
        .font(.system(size: 12))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 18)
        .frame(height: PanelMetrics.footerHeight)
        .contentShape(Rectangle())
        .onTapGesture { model.openResults() }
    }
}

private struct ResultRow: View {
    let hit: FileHit
    let selected: Bool

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon(for: hit.kind))
                .font(.system(size: 18))
                .foregroundStyle(selected ? Color.white : .secondary)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(hit.name)
                    .font(.system(size: 14, weight: .medium))
                    .lineLimit(1)
                Text(parentPath(hit.path))
                    .font(.system(size: 11))
                    .foregroundStyle(selected ? Color.white.opacity(0.8) : .secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            TagDots(tags: hit.tags ?? [])
        }
        .foregroundStyle(selected ? Color.white : .primary)
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(selected ? Color.accentColor : Color.clear)
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
        )
    }

    private func parentPath(_ path: String) -> String {
        (path as NSString).deletingLastPathComponent
    }

    private func icon(for kind: String) -> String {
        switch kind {
        case "image": return "photo"
        case "audio": return "music.note"
        case "video": return "film"
        case "document": return "doc.text"
        case "code": return "chevron.left.forwardslash.chevron.right"
        case "folder": return "folder"
        case "archive": return "doc.zipper"
        default: return "doc"
        }
    }
}

// The trailing Finder-tag swatches on a result row: one dot per colored tag,
// in the file's tag order. Named tags with no color (index 0) are skipped --
// there's nothing to draw for them in a compact row.
private struct TagDots: View {
    let tags: [FileTag]

    var body: some View {
        HStack(spacing: 4) {
            ForEach(Array(tags.enumerated()), id: \.offset) { _, tag in
                if let color = Self.color(for: tag.color) {
                    Circle()
                        .fill(color)
                        .frame(width: 9, height: 9)
                        .overlay(
                            Circle().stroke(Color.black.opacity(0.18), lineWidth: 0.5))
                        .help(tag.name)
                }
            }
        }
    }

    // Finder label indices → colors (0 = no color, drawn as nothing).
    static func color(for index: Int) -> Color? {
        switch index {
        case 1: return .gray
        case 2: return .green
        case 3: return .purple
        case 4: return .blue
        case 5: return .yellow
        case 6: return .red
        case 7: return .orange
        default: return nil
        }
    }
}

// A removable capsule for one active filter, rendered inside the search field.
// Tapping anywhere on it (including the ×) removes the filter.
private struct FilterChip: View {
    let filter: Filter
    let onRemove: () -> Void

    var body: some View {
        HStack(spacing: 4) {
            if case .tag(let c) = filter, let color = TagDots.color(for: c) {
                Circle().fill(color).frame(width: 8, height: 8)
            }
            Text(FilterLabels.text(filter))
                .font(.system(size: 12, weight: .medium))
                .lineLimit(1)
            Image(systemName: "xmark")
                .font(.system(size: 8, weight: .bold))
                .opacity(0.7)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Capsule().fill(Color.accentColor.opacity(0.22)))
        .contentShape(Capsule())
        .onTapGesture(perform: onRemove)
    }
}

// Human-facing labels for filters. The daemon only ever sees canonical kinds
// (image|audio|video|document); these map them back for chips and menu items.
private enum FilterLabels {
    static func text(_ filter: Filter) -> String {
        switch filter {
        case .recentlyDownloaded: return "Downloaded"
        case .kind(let k): return kindLabel(k)
        case .tag(let c): return colorName(c)
        }
    }

    static func kindLabel(_ kind: String) -> String {
        switch kind {
        case "image": return "Photos"
        case "video": return "Videos"
        case "audio": return "Music"
        case "document": return "Documents"
        case "code": return "Code"
        default: return kind.capitalized
        }
    }

    static func colorName(_ color: Int) -> String {
        switch color {
        case 1: return "Gray"
        case 2: return "Green"
        case 3: return "Purple"
        case 4: return "Blue"
        case 5: return "Yellow"
        case 6: return "Red"
        case 7: return "Orange"
        default: return "Tag"
        }
    }
}
