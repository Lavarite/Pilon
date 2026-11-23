.pragma library

var controlGap = 8
var panelGap = 12
var pageMargin = 16

function luminance(colour) {
    function linear(channel) {
        return channel <= .04045 ? channel / 12.92 : Math.pow((channel + .055) / 1.055, 2.4)
    }
    return linear(colour.r) * .2126 + linear(colour.g) * .7152 + linear(colour.b) * .0722
}

function contrastRatio(first, second) {
    const a = luminance(first), b = luminance(second)
    return (Math.max(a, b) + .05) / (Math.min(a, b) + .05)
}

function contrastingText(colour, opacity) {
    const dark = Qt.rgba(.04, .04, .04, 1), light = Qt.rgba(.96, .96, .96, 1)
    const brightness = luminance(colour)
    const darkContrast = (brightness + .05) / (luminance(dark) + .05)
    const lightContrast = (luminance(light) + .05) / (brightness + .05)
    const alpha = opacity === undefined ? 1 : opacity
    return darkContrast > lightContrast ? Qt.rgba(.04, .04, .04, alpha) : Qt.rgba(.96, .96, .96, alpha)
}
