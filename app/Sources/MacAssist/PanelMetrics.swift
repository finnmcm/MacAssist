import CoreGraphics

// Fixed layout metrics shared by SearchView (which lays out the content)
// and PanelController (which sizes the window). Keeping them in one place is
// what lets the window height match the content exactly — the cause of the
// earlier "cut off / invisible results" bug was the window not growing to
// fit the list.
enum PanelMetrics {
    static let width: CGFloat = 620
    static let fieldHeight: CGFloat = 56
    static let toolbarHeight: CGFloat = 46  // always-visible filter button row
    static let rowHeight: CGFloat = 52
    static let footerHeight: CGFloat = 44
    static let statusHeight: CGFloat = 44
    static let dividerHeight: CGFloat = 1
    static let listVPadding: CGFloat = 8  // 4 top + 4 bottom inside the list
    static let maxVisibleRows = 8          // beyond this the list scrolls

    // Height of the scrollable results region for a given hit count.
    static func listHeight(rowCount: Int) -> CGFloat {
        let visible = min(max(rowCount, 0), maxVisibleRows)
        return CGFloat(visible) * rowHeight + listVPadding
    }

    // The always-present header: search field + filter toolbar row.
    static var headerHeight: CGFloat { fieldHeight + toolbarHeight }

    // Total window height for the current state: header alone when empty,
    // header + list + "show all" footer when there are hits, or header + a
    // status line ("No matches"/error) when there are none but a message.
    static func totalHeight(rowCount: Int, hasStatus: Bool) -> CGFloat {
        if rowCount > 0 {
            return headerHeight + dividerHeight + listHeight(rowCount: rowCount)
                + dividerHeight + footerHeight
        }
        if hasStatus {
            return headerHeight + dividerHeight + statusHeight
        }
        return headerHeight
    }
}
