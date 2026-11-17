.pragma library

var controlGap = 8
var panelGap = 12
var pageMargin = 16

function contrastingText(colour, opacity) {
    function linear(channel) {
        return channel <= .04045 ? channel / 12.92 : Math.pow((channel + .055) / 1.055, 2.4)
    }
    const luminance = linear(colour.r) * .2126 + linear(colour.g) * .7152 + linear(colour.b) * .0722
    const darkContrast = (luminance + .05) / (linear(.04) + .05)
    const lightContrast = (linear(.96) + .05) / (luminance + .05)
    const alpha = opacity === undefined ? 1 : opacity
    return darkContrast > lightContrast ? Qt.rgba(.04, .04, .04, alpha) : Qt.rgba(.96, .96, .96, alpha)
}
