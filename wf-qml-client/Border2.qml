import QtQuick
import QtQuick.Controls
import QtQuick.Effects
Rectangle{
    signal maximize
    signal minimize
    signal close
    signal move
    signal border_1
    signal border_2
    property string title
    property string windowIcon
    property int titleBarHeight
    property int borderWidth
    border.width:1
    color:Qt.rgba(0,0,1,0.5)
    DragHandler{
        id: dragHandler
        target: null
        onActiveChanged:{
            if (active){
                move()
            }
        }
    }
    Rectangle{
        id:innerBorder
        border.width:1
        color:Qt.rgba(0.5,0.5,1,1)
        anchors{fill:parent;topMargin:titleBarHeight-1;leftMargin:borderWidth-1;rightMargin:borderWidth-1;bottomMargin:borderWidth}
    }
    Menu{
        id:testMenu
        popupType:Menu.Window
        background:Rectangle{
            border.width:1
            color:Qt.rgba(0.7,0.7,0.7,0.5)
        }
        width:400
        title:'TestMenu'
        MenuItem{
            text:'Border 1'
            onClicked:border_1()
        }
        MenuItem{
            text:'Border 2'
            onClicked:border_2()
        }
    }

    Item{
        id:buttonContainer
        anchors{top:parent.top;left:parent.left;right:parent.right;bottom:innerBorder.top;margins:1}
        Button{
            id:icon
            anchors{top:parent.top;bottom:parent.bottom;left:parent.left;margins:4}
            width:height
            background:Item{}
            display:AbstractButton.IconOnly
            enabled:false
            icon{
                color:'transparent'
                width:32
                height:32
                name:windowIcon
            }
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:3
                    shadowVerticalOffset:3
                }
            }
        }
        Button{
            id:menuButton
            anchors{top:parent.top;bottom:parent.bottom;left:icon.right;margins:8}
            background:Rectangle{
                color:'blue'
                radius:8
                border.width:1
                Rectangle{
                    anchors{fill:parent;margins:1}
                    radius:8
                }
            }
            width:height
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:2
                    shadowVerticalOffset:2
                }
            }
            onClicked:testMenu.popup()
        }
        Text{
            anchors{top:parent.top;bottom:parent.bottom;horizontalCenter:parent.horizontalCenter}
            color:'white'
            width:400
            horizontalAlignment:Text.AlignHCenter
            verticalAlignment:Text.AlignVCenter
            text:title
            style:Text.Outline
            styleColor:Qt.rgba(0,0.5,0.5,1)
            renderType:TextInput.CurveRendering
            font.bold:true
            font.pointSize:16
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:2
                    shadowVerticalOffset:2
                    shadowColor:'cyan'
                }
            }
        }
        Button{
            id:closeButton
            anchors{top:parent.top;bottom:parent.bottom;right:parent.right;margins:8}
            background:Rectangle{
                color:'red'
                radius:8
                border.width:1
                Rectangle{
                    anchors{fill:parent;margins:1}
                    radius:8
                }
            }
            width:height
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:2
                    shadowVerticalOffset:2
                }
            }
            onClicked:close()
        }
        Button{
            id:maxButton
            anchors{top:parent.top;bottom:parent.bottom;right:closeButton.left;margins:8}
            background:Rectangle{
                color:'green'
                radius:8
                border.width:1
                Rectangle{
                    anchors{fill:parent;margins:1}
                    radius:8
                }
            }
            width:height
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:2
                    shadowVerticalOffset:2
                }
            }
            onClicked:maximize()
        }
        Button{
            id:minButton
            anchors{top:parent.top;bottom:parent.bottom;right:maxButton.left;margins:8}
            background:Rectangle{
                color:'yellow'
                radius:8
                border.width:1
                Rectangle{
                    anchors{fill:parent;margins:1}
                    radius:8
                }
            }
            width:height
            layer{
                enabled:true
                smooth:true
                effect:MultiEffect{
                    shadowEnabled:true
                    shadowHorizontalOffset:2
                    shadowVerticalOffset:2
                }
            }
            onClicked:minimize()
        }
    }
}
