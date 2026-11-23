import { icons } from "./catalog.mjs"
import Fuse from "./fuse.mjs"

// Match the ranking and query syntax of phosphoricons.com.
let index
const catalogue = [...icons, {name: "pilon", tags: ["application", "logo"], categories: ["brands"]}]
const names = catalogue.map(icon => icon.name)
WorkerScript.onMessage = function(query) {
    let results = names
    if (query) {
        if (!index) index = new Fuse(catalogue, {
            keys: [{name: "name", weight: 4}, "tags", "categories", "codepoint"],
            threshold: 0.2,
            useExtendedSearch: true
        })
        results = index.search(query).map(hit => hit.item.name)
    }
    WorkerScript.sendMessage({query, names: results})
}
