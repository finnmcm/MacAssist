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
            TextField("Search your files…", text: $model.queryText)
                .textFieldStyle(.plain)
                .font(.system(size: 20, weight: .regular))
                .focused($fieldFocused)
        }
        .padding(.horizontal, 18)
        .frame(height: PanelMetrics.fieldHeight)
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
                            .onTapGesture { model.reveal(model.results[index]) }
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
        default: return "doc"
        }
    }
}
