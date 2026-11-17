import QtQuick

QtObject {
    id: palette
    required property color accentColour
    required property color mainColour
    // Store the swatch index, so a theme colour follows later theme changes.
    readonly property var suggestions: {
        const dark = mainColour.r * .2126 + mainColour.g * .7152 + mainColour.b * .0722 < .5
        const hue = accentColour.hslHue >= 0 ? accentColour.hslHue : Math.max(0, mainColour.hslHue)
        const saturation = Math.max(.22, Math.min(.65, accentColour.hslSaturation))
        const lightness = dark ? .66 : .38
        return [0, .08, -.08, .2, -.2, .5].map(offset => Qt.hsla((hue + offset + 1) % 1, saturation, lightness, 1))
    }
    function resolve(value) {
        const match = /^theme:([0-5])$/.exec(value || "")
        return match ? suggestions[Number(match[1])] : value || accentColour
    }
    function onColour(colour) {
        function linear(channel) { return channel <= .04045 ? channel / 12.92 : Math.pow((channel + .055) / 1.055, 2.4) }
        const luminance = linear(colour.r) * .2126 + linear(colour.g) * .7152 + linear(colour.b) * .0722
        return (luminance + .05) / (linear(10 / 255) + .05) > (linear(245 / 255) + .05) / (luminance + .05)
            ? "#0a0a0a" : "#f5f5f5"
    }
}
