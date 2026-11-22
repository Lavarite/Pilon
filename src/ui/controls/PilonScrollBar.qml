import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ScrollBar {
    // The Basic style fades the thumb but keeps its hit area visible when content fits.
    visible: policy === Basic.ScrollBar.AlwaysOn
        || (policy === Basic.ScrollBar.AsNeeded && size > 0
            && (vertical ? height : width) * (1 / size - 1) > 0.5)
}
