import SwiftUI

// The panel's content: a search field over a ranked results list. Keyboard
// navigation (arrows, Enter, Esc) is handled by a key monitor in
// PanelController so it works regardless of SwiftUI focus quirks; this view
// stays presentational.
struct SearchView: View {
    @ObservedObject var model: SearchModel
    @FocusState private var fieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
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
            .frame(height: 56)

            if !model.results.isEmpty || !model.status.isEmpty {
                Divider()
            }

            if !model.status.isEmpty && model.results.isEmpty {
                Text(model.status)
                    .foregroundStyle(.secondary)
                    .font(.system(size: 13))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 18)
                    .padding(.vertical, 12)
            }

            if !model.results.isEmpty {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            ForEach(Array(model.results.enumerated()), id: \.element.id) { index, hit in
                                ResultRow(hit: hit, selected: index == model.selection)
                                    .id(index)
                                    .contentShape(Rectangle())
                                    .onTapGesture { model.open(hit) }
                            }
                        }
                        .padding(.vertical, 6)
                    }
                    .frame(maxHeight: 320)
                    .onChange(of: model.selection) { _, newValue in
                        withAnimation(.easeOut(duration: 0.12)) {
                            proxy.scrollTo(newValue, anchor: .center)
                        }
                    }
                }
            }
        }
        .frame(width: 620)
        .onAppear { fieldFocused = true }
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
        .padding(.vertical, 8)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(selected ? Color.accentColor : Color.clear)
        )
        .padding(.horizontal, 10)
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
