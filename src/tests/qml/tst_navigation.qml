import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "PageNavigation"
    width: 800; height: 600; visible: true; when: windowShown
    Component { id: pageComponent; Item { width: 100; height: 100; visible: false } }
    Component {
        id: navigationComponent
        App.AppNavigation { id: bar; width: 150; height: 240; onActivated: pageId => currentId = pageId }
    }
    Component {
        id: reorderComponent
        App.AppNavigation {
            x: 100; y: 100; width: 190; height: 240
            expanded: true; reorderEnabled: true; visibilityControls: true
            onReorderRequested: (from, to) => {
                const next = pages.slice(), moved = next.splice(from, 1)[0]
                next.splice(to, 0, moved); pages = next
            }
            onPageVisibilityRequested: (id, visible) => {
                pages = pages.map(page => page.id === id ? Object.assign({}, page, {sidebarVisible:visible}) : page)
            }
        }
    }
    function test_visibilityControlsStayInsideAccessibleParentBounds() {
        const bar = createTemporaryObject(reorderComponent, test)
        bar.pages = [{id:"first",title:"First",sidebarVisible:true},{id:"second",title:"Second",sidebarVisible:true}]
        waitForPolish(bar)
        const toggle = findChild(bar,"sidebarVisible-first")
        verify(toggle)
        verify(toggle.x >= 0 && toggle.x + toggle.width <= toggle.parent.width,
            "Visibility controls remain inside their parent so assistive technology can reach them")
        tryVerify(() => findChild(bar,"sidebarVisible-second").mapToItem(bar,0,0).y > toggle.mapToItem(bar,0,0).y)
        mouseClick(toggle,toggle.width/2,toggle.height/2)
        tryVerify(() => bar.pages[0].sidebarVisible === false)
        const restored = findChild(bar,"sidebarVisible-first")
        restored.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(bar.pages[0].sidebarVisible,true)
        compare(bar.pages.map(page=>page.id),["first","second"])
    }
    function test_draggedPageStaysInsideViewportAtBothEnds() {
        const bar = createTemporaryObject(reorderComponent, test)
        verify(bar)
        const pages = []
        for (let i = 0; i < 6; ++i) pages.push({id:"page"+i,title:"Page "+i,sidebarVisible:true})
        bar.pages = pages
        waitForPolish(bar)
        const scroll = findChild(bar,"sidebarPreviewScroll")
        tryVerify(() => scroll.contentHeight > scroll.height)
        scroll.contentY = scroll.contentHeight - scroll.height
        const last = findChild(bar,"page5PreviewNavigation")
        const start = last.mapToItem(bar,last.width/2,last.height/2)
        mousePress(bar,start.x,start.y)
        mouseMove(bar,start.x,start.y-Qt.styleHints.startDragDistance-2,20)
        mouseMove(bar,start.x,-40,20)
        tryVerify(() => bar.dragging)
        function withinViewport() {
            const top = bar.draggedItem.mapToItem(scroll,0,0).y
            return top >= -1 && top + bar.draggedItem.height <= scroll.height + 1
        }
        verify(withinViewport(),"The full row stays below the header while dragging above the viewport")
        tryCompare(scroll,"contentY",0)
        tryCompare(bar,"dragTargetIndex",0)
        verify(withinViewport(),"Autoscrolling to the top preserves the row's visible bounds")
        mouseRelease(bar,start.x,-40)
        compare(bar.pages[0].id,"page5","Dropping above the viewport still moves the page to the top")
        bar.Window.window.update(); verify(waitForRendering(bar.Window.window.contentItem))
        const first = findChild(bar,"page5PreviewNavigation")
        const firstStart = first.mapToItem(bar,first.width/2,first.height/2)
        mousePress(bar,firstStart.x,firstStart.y)
        mouseMove(bar,firstStart.x,firstStart.y+Qt.styleHints.startDragDistance+2,20)
        mouseMove(bar,firstStart.x,bar.height+40,20)
        tryVerify(() => bar.dragging)
        verify(withinViewport(),"The full row stays above the footer while dragging below the viewport")
        tryCompare(scroll,"contentY",scroll.contentHeight-scroll.height)
        tryCompare(bar,"dragTargetIndex",bar.pages.length-1)
        verify(withinViewport())
        mouseRelease(bar,firstStart.x,bar.height+40)
        compare(bar.pages[bar.pages.length-1].id,"page5")
    }
    function test_viewportResizeCancelsDragWithoutSavingThePreviewOrder() {
        const bar=createTemporaryObject(reorderComponent,test)
        bar.pages=[{id:"first",title:"First"},{id:"second",title:"Second"},{id:"third",title:"Third"}]
        waitForPolish(bar)
        const first=findChild(bar,"firstPreviewNavigation")
        const start=first.mapToItem(bar,first.width/2,first.height/2)
        mousePress(bar,start.x,start.y);mouseMove(bar,start.x,start.y+110,20)
        tryVerify(()=>bar.dragging);tryCompare(bar,"dragTargetIndex",2)
        bar.width=220
        verify(!bar.dragging)
        mouseRelease(bar,start.x,start.y+110)
        compare(bar.pages.map(page=>page.id),["first","second","third"])
        waitForPolish(bar)
        const fresh=findChild(bar,"firstPreviewNavigation")
        const next=fresh.mapToItem(bar,fresh.width/2,fresh.height/2)
        mouseDrag(bar,next.x,next.y,0,110)
        compare(bar.pages.map(page=>page.id),["second","third","first"])
    }
    function test_swipeAndHeldReorderShareTheHorizontalPreview() {
        const bar = createTemporaryObject(reorderComponent, test, {width:180, height:90, horizontal:true})
        const pages = Array.from({length:9}, (_, i) => ({id:"page"+i, title:"Page "+i}))
        bar.pages = pages; bar.currentId = pages[0].id
        const scroll = findChild(bar, "sidebarPreviewScroll")
        tryVerify(() => scroll.contentWidth > scroll.width)
        verify(waitForRendering(bar))
        let activated = ""
        bar.activated.connect(id => activated = id)
        const tap = touchEvent(bar)
        tap.press(0, bar, 22, 22).commit()
        tap.release(0, bar, 22, 22).commit()
        compare(activated, "page0", "A quick tap still activates the tab")
        const swipe = touchEvent(bar)
        swipe.press(0, bar, 145, 22).commit()
        swipe.move(0, bar, 100, 22).commit()
        swipe.move(0, bar, 30, 22).commit()
        swipe.release(0, bar, 30, 22).commit()
        tryVerify(() => scroll.contentX > 10)
        compare(bar.pages, pages, "A swipe scrolls without reordering")
        compare(bar.dragging, false)
        scroll.cancelFlick(); scroll.contentX = 0
        const drag = touchEvent(bar)
        drag.press(0, bar, 22, 22).commit()
        tryVerify(() => bar.dragging, 400, "A short hold starts reordering")
        drag.move(0, bar, bar.width - 8, 22).commit()
        tryVerify(() => scroll.contentX > 50)
        tryVerify(() => bar.dragTargetIndex > 3)
        drag.release(0, bar, bar.width - 8, 22).commit()
        verify(bar.pages.findIndex(page => page.id === "page0") > 3)
        verify(!bar.dragging)
    }
    function test_sixthPageNeedsOnlyADescriptor() {
        const bar = createTemporaryObject(navigationComponent, test)
        verify(bar)
        let pages = [], items = []
        for (let i = 0; i < 6; ++i) {
            const item = createTemporaryObject(pageComponent, test)
            items.push(item)
            pages.push({id: "page" + i, title: "Page " + i, page: item})
        }
        bar.pages = pages; bar.currentId = "page0"
        waitForPolish(bar)
        const scroll = findChild(bar, "sidebarScroll")
        tryVerify(() => scroll.contentHeight > scroll.height)
        const last = findChild(bar, "page5Navigation")
        verify(last)
        scroll.contentY = scroll.contentHeight - scroll.height
        mouseClick(last)
        compare(bar.currentId, "page5")
        for (const item of items) compare(item.visible, false, "Navigation does not own page visibility")
        mouseClick(last)
        verify(last.checked, "Clicking a selected page keeps it selected")
        last.forceActiveFocus(); keyClick(Qt.Key_Up)
        verify(findChild(bar, "page4Navigation").activeFocus)
        keyClick(Qt.Key_Space)
        compare(bar.currentId, "page4")
    }
    function test_bottomNavigationScrollsAndKeepsFocusOnReflow() {
        const bar = createTemporaryObject(navigationComponent, test)
        bar.pages = Array.from({length: 9}, (_, index) => ({id: "page" + index, title: "Page " + index}))
        bar.currentId = "page0"
        const first = findChild(bar, "page0Navigation")
        first.forceActiveFocus()
        bar.horizontal = true
        bar.height = 44
        tryVerify(() => first.activeFocus)
        const scroll = findChild(bar, "sidebarScroll")
        tryVerify(() => scroll.contentWidth > scroll.width && scroll.interactive)
        compare(scroll.contentHeight, scroll.height)
        verify(!findChild(bar, "sidebarToggle").visible)
        waitForRendering(bar)
        const swipe = touchEvent(bar)
        swipe.press(0, bar, 130, 20).commit()
        swipe.move(0, bar, 95, 20).commit()
        swipe.move(0, bar, 30, 20).commit()
        swipe.release(0, bar, 30, 20).commit()
        tryVerify(() => scroll.contentX > 0)
        compare(bar.currentId, "page0")
        first.forceActiveFocus()
        keyClick(Qt.Key_Left)
        const last = findChild(bar, "page8Navigation")
        verify(last.activeFocus)
        tryVerify(() => last.mapToItem(scroll, last.width, 0).x <= scroll.width + 1)
        keyClick(Qt.Key_Space)
        compare(bar.currentId, "page8")
        bar.horizontal = false
        bar.height = 240
        tryVerify(() => last.activeFocus && last.checked)
        keyClick(Qt.Key_Up)
        verify(findChild(bar, "page7Navigation").activeFocus)
    }
}
