.pragma library

function partialString(text, field) {
    const match = new RegExp('(?:^|[,{])\\s*"' + field + '"\\s*:\\s*"').exec(text)
    if (!match) return null
    let result = ""
    for (let i = match.index + match[0].length; i < text.length; ++i) {
        const character = text[i]
        if (character === '"') return result
        if (character !== "\\") { result += character; continue }
        if (++i >= text.length) break
        const escaped = text[i]
        if (escaped === "u") {
            const hex = text.slice(i + 1, i + 5)
            if (!/^[0-9a-fA-F]{4}$/.test(hex)) break
            result += String.fromCharCode(parseInt(hex, 16))
            i += 4
        } else {
            const escapes = {n: "\n", r: "\r", t: "\t", b: "\b", f: "\f", '"': '"', "\\": "\\", "/": "/"}
            if (escapes[escaped] === undefined) break
            result += escapes[escaped]
        }
    }
    return result
}

function toolText(text, name) {
    if (!text) return ""
    let value
    try { value = JSON.parse(text) } catch (error) {
        // Tool argument tokens arrive before their JSON string is closed.
        // Decode available code immediately instead of displaying escaped \n.
        if (name === "create_artifact" || name === "update_artifact" || name === "local_exec" || name === "ssh_exec") {
            const field = name.endsWith("_exec") ? "command" : "content"
            const source = partialString(text, field)
            if (source !== null) {
                const path = partialString(text, "filename")
                return (path ? JSON.stringify({filename: path}, null, 2) + "\n\n" : "") + source
            }
        }
        return text
    }
    if (value === null || typeof value !== "object" || Array.isArray(value))
        return JSON.stringify(value, null, 2)
    const metadata = Object.assign({}, value)
    const blocks = []
    for (const field of ["command", "content", "output", "text", "instructions"]) {
        if (typeof value[field] !== "string") continue
        delete metadata[field]
        if (value[field].length)
            blocks.push(value[field])
    }
    if (Object.keys(metadata).length)
        blocks.unshift(JSON.stringify(metadata, null, 2))
    return blocks.length ? blocks.join("\n\n") : JSON.stringify(value, null, 2)
}
