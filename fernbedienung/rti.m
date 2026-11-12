/* iPhone-Tastatur (RTI): Archive wie tvOS bauen und die Texteingaben des iPhones lesen.
   Die Klassen heissen wie bei Apple (auf Leopard gibt es keine davon), Apples NSKeyedArchiver
   erzeugt das Format (gleiche Texte zusammengefasst, Reihenfolge wie bei Apple).
   setClassName:forClass: am Archivierer stuerzt unter 10.5 ab, daher die echten Namen. */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include "rti.h"

/* RTIKeyedArchiver: so heisst der Archivierer auf Apple-TV und iPhone ($archiver) */
@interface RTIKeyedArchiver : NSKeyedArchiver
@end
@implementation RTIKeyedArchiver
@end

@interface NSUUID : NSObject <NSCoding> {
@public
    unsigned char b[16];
    int gelesen;
}
@end
@implementation NSUUID
- (void)encodeWithCoder:(NSCoder *)c { [c encodeBytes:b length:16 forKey:@"NS.uuidbytes"]; }
- (id)initWithCoder:(NSCoder *)c
{
    if ((self = [super init])) {
        NSUInteger n = 0;
        const uint8_t *p = [c decodeBytesForKey:@"NS.uuidbytes" returnedLength:&n];
        if (p && n == 16) { memcpy(b, p, 16); gelesen = 1; }
    }
    return self;
}
@end

/* TIDocumentState: Text um die Einfuegemarke */
@interface TIDocumentState : NSObject <NSCoding> {
@public
    NSString *vor, *auswahl, *nach;
}
@end
@implementation TIDocumentState
- (void)encodeWithCoder:(NSCoder *)c
{
    if (vor) [c encodeObject:vor forKey:@"contextBeforeInput"];
    if (auswahl) [c encodeObject:auswahl forKey:@"selectedText"];
    if (nach) [c encodeObject:nach forKey:@"contextAfterInput"];
}
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
- (void)dealloc { [vor release]; [auswahl release]; [nach release]; [super dealloc]; }
@end

/* RTIDocumentState */
@interface RTIDocumentState : NSObject <NSCoding> {
@public
    TIDocumentState *docSt;
}
@end
@implementation RTIDocumentState
- (void)encodeWithCoder:(NSCoder *)c { [c encodeObject:docSt forKey:@"docSt"]; }
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
- (void)dealloc { [docSt release]; [super dealloc]; }
@end

/* RTIDocumentTraits: Titel des Feldes */
@interface RTIDocumentTraits : NSObject <NSCoding> {
@public
    NSString *titel;
}
@end
@implementation RTIDocumentTraits
- (void)encodeWithCoder:(NSCoder *)c { if (titel) [c encodeObject:titel forKey:@"title"]; }
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
- (void)dealloc { [titel release]; [super dealloc]; }
@end

/* RTITextOperations und TIKeyboardOutput (vom iPhone) */
@interface TIKeyboardOutput : NSObject <NSCoding> {
@public
    NSString *einfuegen;
    int loeschen;
}
@end
@implementation TIKeyboardOutput
- (void)encodeWithCoder:(NSCoder *)c { }
- (id)initWithCoder:(NSCoder *)c
{
    if ((self = [super init])) {
        id t = [c decodeObjectForKey:@"insertionText"];
        if ([t isKindOfClass:[NSString class]]) einfuegen = [t retain];
        if ([c containsValueForKey:@"deletionCount"]) loeschen = [c decodeIntForKey:@"deletionCount"];
    }
    return self;
}
- (void)dealloc { [einfuegen release]; [super dealloc]; }
@end

@interface RTITextOperations : NSObject <NSCoding> {
@public
    TIKeyboardOutput *ausgabe;
    NSString *zusicherung;
    NSUUID *ziel;
    int zusichern, tastatur;
}
@end
@implementation RTITextOperations
- (void)encodeWithCoder:(NSCoder *)c { }
- (id)initWithCoder:(NSCoder *)c
{
    if ((self = [super init])) {
        id a = [c decodeObjectForKey:@"keyboardOutput"], z = [c decodeObjectForKey:@"textToAssert"], u = [c decodeObjectForKey:@"targetSessionUUID"];
        if ([a isKindOfClass:[TIKeyboardOutput class]]) ausgabe = [a retain];
        if ([z isKindOfClass:[NSString class]]) { zusicherung = [z retain]; zusichern = 1; }
        if ([u isKindOfClass:[NSUUID class]]) ziel = [u retain];
        tastatur = [c containsValueForKey:@"inputSourceState"];
    }
    return self;
}
- (void)dealloc { [ausgabe release]; [zusicherung release]; [ziel release]; [super dealloc]; }
@end

/* unbekannte Klassen im Archiv: leer lesen statt abbrechen */
@interface FBEgal : NSObject <NSCoding>
@end
@implementation FBEgal
- (void)encodeWithCoder:(NSCoder *)c { }
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
@end

@interface FBLeser : NSObject
@end
@implementation FBLeser
- (Class)unarchiver:(NSKeyedUnarchiver *)u cannotDecodeObjectOfClassName:(NSString *)n originalClasses:(NSArray *)k
{
    return [FBEgal class];
}
@end

static NSString *text(const char *s) { return s && *s ? [NSString stringWithUTF8String:s] : nil; }

int rti_archiv(unsigned char **aus, const unsigned char uuid[16], const char *titel,
               const char *vor, const char *auswahl, const char *nach)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSMutableData *d = [NSMutableData data];
    RTIKeyedArchiver *a = [[[RTIKeyedArchiver alloc] initForWritingWithMutableData:d] autorelease];
    NSUUID *u = [[[NSUUID alloc] init] autorelease];
    RTIDocumentTraits *z = [[[RTIDocumentTraits alloc] init] autorelease];
    RTIDocumentState *rd = [[[RTIDocumentState alloc] init] autorelease];
    TIDocumentState *td = [[TIDocumentState alloc] init];
    int n = 0;

    memcpy(u->b, uuid, 16);
    z->titel = [text(titel) retain];
    td->vor = [(text(vor) ? text(vor) : @"") retain];
    td->auswahl = [text(auswahl) retain];
    td->nach = [text(nach) retain];
    rd->docSt = td;
    [a setOutputFormat:NSPropertyListBinaryFormat_v1_0];
    @try {
#ifdef PRUEF
        fprintf(stderr, "a=%p u=%p z=%p rd=%p td=%p\n", a, u, z, rd, td);
#endif
        [a encodeObject:u forKey:@"sessionUUID"];
#ifdef PRUEF
        fprintf(stderr, "uuid ok\n");
#endif
        [a encodeObject:z forKey:@"documentTraits"];
#ifdef PRUEF
        fprintf(stderr, "zuege ok\n");
#endif
        [a encodeObject:rd forKey:@"documentState"];
#ifdef PRUEF
        fprintf(stderr, "zustand ok\n");
#endif
        [a finishEncoding];
        n = (int)[d length];
        *aus = malloc(n);
        memcpy(*aus, [d bytes], n);
    } @catch (NSException *e) {
        n = 0;
    }
    [pool release];
    return n;
}

int rti_lesen(const unsigned char *d, int n, RtiEingabe *e, char *roh, int rmax)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSData *daten = [NSData dataWithBytes:d length:n];
    FBLeser *leser = [[[FBLeser alloc] init] autorelease];
    int r = -1;
    memset(e, 0, sizeof *e);
    if (roh && rmax > 0) {
        NSString *f = nil;
        id p = [NSPropertyListSerialization propertyListFromData:daten mutabilityOption:NSPropertyListImmutable format:NULL errorDescription:&f];
        NSString *s = p ? [p description] : @"?";
        s = [[s componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] componentsJoinedByString:@" "];
        strncpy(roh, [s UTF8String], rmax - 1);
        roh[rmax - 1] = 0;
    }
    @try {
        NSKeyedUnarchiver *u = [[[NSKeyedUnarchiver alloc] initForReadingWithData:daten] autorelease];
        id o;
        [u setDelegate:leser];
        o = [u decodeObjectForKey:@"textOperations"];
        if ([o isKindOfClass:[RTITextOperations class]]) {
            RTITextOperations *ops = o;
            if (ops->zusichern) {
                e->zusichern = 1;
                strncpy(e->zusicherung, [ops->zusicherung UTF8String], sizeof e->zusicherung - 1);
            }
            if (ops->ausgabe) {
                if (ops->ausgabe->einfuegen) strncpy(e->einfuegen, [ops->ausgabe->einfuegen UTF8String], sizeof e->einfuegen - 1);
                e->loeschen = ops->ausgabe->loeschen;
            }
            if (ops->ziel && ops->ziel->gelesen) { memcpy(e->uuid, ops->ziel->b, 16); e->hat_uuid = 1; }
            e->von_tastatur = ops->tastatur;
            r = 0;
        }
        [u finishDecoding];
    } @catch (NSException *x) {
        r = -1;
    }
    [pool release];
    return r;
}
