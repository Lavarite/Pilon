import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "NotePaperSettings"
    width: 600; height: 600; visible: true; when: windowShown
    property var savedPaper: null
    property bool acceptedSave: true
    property string saveError: ""
    property bool savedDefault: false
    ApplicationWindow {
        id: owner
        property real uiScale: 1
        width: 500; height: 600; visible: true
        palette.window: "#202020"; palette.button: "#181818"; palette.text: "#eeeeee"
        palette.windowText: "#eeeeee"; palette.buttonText: "#eeeeee"; palette.highlight: "#aabbcc"; palette.highlightedText: "#101010"
        Item { id: surface; width: owner.width/owner.uiScale; height: owner.height/owner.uiScale; scale: owner.uiScale; transformOrigin: Item.TopLeft }
    }
    App.NotePaperDialog {
        id: dialog
        parent: surface; palette: owner.palette
        errorMessage: test.saveError
        savePaper: (value, useDefault) => {
            if (!test.acceptedSave) { test.saveError="Cannot save the page template."; return false }
            test.savedPaper=value; test.savedDefault=useDefault; return true
        }
    }
    function control(name) { const item=findChild(dialog,name); verify(item!==null,name); return item }
    function reveal(item) {
        const view=dialog.contentItem, flickable=view.contentItem
        dialog.dialogWindow.update()
        verify(waitForRendering(view))
        const point=item.mapToItem(flickable.contentItem,0,0)
        flickable.contentY=Math.max(0,Math.min(flickable.contentHeight-flickable.height,point.y-(flickable.height-item.height)/2))
        dialog.dialogWindow.update()
        verify(waitForRendering(view))
        tryVerify(()=> {
            const point=item.mapToItem(view,0,0)
            return point.x>=0 && point.y>=0 && point.x+item.width<=view.width+1 && point.y+item.height<=view.height+1
        })
    }
    function click(name) { const item=control(name); reveal(item); mouseClick(item) }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        savedPaper=null; savedDefault=false; acceptedSave=true; saveError=""
        dialog.hasNote=false
        dialog.show({pattern:"blank",background:"#ffffff",lines:"#d9d9d9",spacingX:24,spacingY:24,lineWidth:.75,size:"A4"})
        tryCompare(dialog,"opened",true); verify(waitForRendering(dialog.contentItem))
    }
    function cleanup() { dialog.close() }
    function test_defaultTemplateNeedsNoNotesPage() {
        const pattern=control("notePaperPattern")
        pattern.currentIndex=2; pattern.activated(2)
        dialog.spacingX=36; dialog.paperSize="A5"
        click("noteApplyPaper")
        tryCompare(dialog,"opened",false)
        verify(savedDefault); compare(savedPaper.pattern,"grid"); compare(savedPaper.spacingX,36); compare(savedPaper.size,"A5")
    }
    function test_failedSaveKeepsDraftAndErrorVisible() {
        acceptedSave=false; dialog.pattern="lined"
        click("noteApplyPaper")
        verify(dialog.opened); compare(dialog.pattern,"lined")
        const error=control("notePaperError"); reveal(error)
        verify(error.visible); compare(error.text,"Cannot save the page template.")
    }
    function test_currentNoteCanApplyOrSetDefault_data() { return [{tag:"apply",control:"noteApplyPaper",expected:false},{tag:"default",control:"noteUseDefaultPaper",expected:true}] }
    function test_currentNoteCanApplyOrSetDefault(data) {
        dialog.hasNote=true; dialog.pattern="grid"
        click(data.control); tryCompare(dialog,"opened",false)
        compare(savedDefault,data.expected)
    }
    function test_nestedColourPickerKeepsTemplateOpen() {
        click("notePaperBackgroundColour")
        const colour=control("notePaperColourDialog")
        tryCompare(colour,"visible",true)
        compare(colour.transientParent,dialog.dialogWindow)
        mouseClick(findChild(colour,"windowCloseButton"))
        tryCompare(colour,"visible",false); verify(dialog.opened)
    }
    function test_responsiveDefaultPaperControls_data() {
        return [{tag:"phone",width:320,height:360,scale:1,font:14},
                {tag:"tablet",width:768,height:600,scale:1.4,font:20},
                {tag:"desktop",width:1366,height:768,scale:1,font:14}]
    }
    function test_responsiveDefaultPaperControls(data) {
        dialog.close(); owner.uiScale=data.scale; owner.width=data.width; owner.height=data.height
        dialog.font=Qt.font({family:Qt.application.font.family,pixelSize:data.font})
        tryVerify(()=>owner.contentItem.width === owner.width && owner.contentItem.height === owner.height)
        dialog.show({pattern:"grid",background:"#ffffff",lines:"#d9d9d9",spacingX:24,spacingY:24,lineWidth:.75,size:"A4"})
        tryCompare(dialog,"opened",true)
        for(const name of ["notePaperPattern","notePaperSize","noteGridSpacing","noteLineSpacing","noteLineThickness","notePaperBackgroundColour","notePaperLineColour","noteApplyPaper"]) {
            const item=control(name); reveal(item)
            const start=item.mapToItem(dialog.dialogWindow.contentItem,0,0)
            const end=item.mapToItem(dialog.dialogWindow.contentItem,item.width,item.height)
            verify(start.x>=0 && start.y>=0 && end.x<=dialog.dialogWindow.width+1 && end.y<=dialog.dialogWindow.height+1,name)
        }
        dialog.dialogWindow.update(); verify(waitForRendering(dialog.contentItem))
        verify(tabletInput.saveWindow(dialog.dialogWindow.contentItem,"/tmp/pilon-note-default-paper-"+data.tag+".png"))
        click("noteApplyPaper"); tryCompare(dialog,"opened",false); verify(savedDefault)
        owner.uiScale=1; owner.width=500; owner.height=600
        dialog.font=Qt.application.font
    }

}
