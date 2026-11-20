#include "native-platform-shell.h"

#import <AppKit/AppKit.h>
#include <QFile>

@interface WorkspaceDockProgressView : NSView
@property(nonatomic, strong) NSView *previousContent;
@property(nonatomic, strong) NSImageView *icon;
@property(nonatomic, strong) NSProgressIndicator *progress;
@end

@implementation WorkspaceDockProgressView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _icon = [[NSImageView alloc] initWithFrame:self.bounds];
        _icon.imageScaling = NSImageScaleProportionallyUpOrDown;
        _icon.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [self addSubview:_icon];
        _progress = [[NSProgressIndicator alloc] initWithFrame:
            NSMakeRect(frame.size.width * 0.1, 2, frame.size.width * 0.8, 12)];
        _progress.indeterminate = NO;
        _progress.style = NSProgressIndicatorStyleBar;
        _progress.minValue = 0;
        _progress.maxValue = 1;
        _progress.autoresizingMask = NSViewWidthSizable;
        [self addSubview:_progress];
    }
    return self;
}
@end

namespace workspace {
namespace {
NSImage *dockIcon(const QString &resource)
{
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) return nil;
    auto bytes = file.readAll();
    return [[NSImage alloc] initWithData:[NSData dataWithBytes:bytes.constData() length:bytes.size()]];
}
}

void setPlatformUnread(QWindow *, bool unread, bool inCall, const QString &)
{
    static NSImage *const normalIcon = dockIcon(QStringLiteral(":/workspace/tray/dock-icon.png"));
    static NSImage *const unreadIcon = dockIcon(QStringLiteral(":/workspace/tray/dock-icon-unread.png"));
    NSImage *icon = unread && !inCall ? unreadIcon : normalIcon;
    if (icon && icon != NSApp.applicationIconImage)
        NSApp.applicationIconImage = icon;
    NSDockTile *tile = NSApp.dockTile;
    tile.badgeLabel = inCall ? @"📞" : @"";
    if ([tile.contentView isKindOfClass:WorkspaceDockProgressView.class])
        ((WorkspaceDockProgressView *)tile.contentView).icon.image = NSApp.applicationIconImage;
    [tile display];
}

void setPlatformTaskbarProgress(QWindow *, qreal fraction)
{
    auto *tile = NSApp.dockTile;
    if (fraction < 0) {
        if ([tile.contentView isKindOfClass:WorkspaceDockProgressView.class])
            tile.contentView = ((WorkspaceDockProgressView *)tile.contentView).previousContent;
    } else {
        WorkspaceDockProgressView *view;
        if ([tile.contentView isKindOfClass:WorkspaceDockProgressView.class]) {
            view = (WorkspaceDockProgressView *)tile.contentView;
        } else {
            view = [[WorkspaceDockProgressView alloc] initWithFrame:NSMakeRect(0, 0, tile.size.width, tile.size.height)];
            view.previousContent = tile.contentView;
            tile.contentView = view;
        }
        view.icon.image = NSApp.applicationIconImage;
        view.progress.doubleValue = fraction;
    }
    [tile display];
}

void setPlatformAttention(quintptr windowId, bool requested)
{
    if (requested && !windowId) return;
    static NSInteger request = -1;
    if (request != -1) {
        [NSApp cancelUserAttentionRequest:request];
        request = -1;
    }
    if (requested) request = [NSApp requestUserAttention:NSInformationalRequest];
}
}
