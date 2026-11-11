/* Rasterize real Unicode glyphs using the game's unmodified x86 FreeType. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL FreeType: %s\n",message);exit(91);}}
int main(int argc,char **argv) {
    require(argc==3,"driver and font paths");void *driver=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(driver!=NULL,"load game x86 FreeType");
#define FN(result,name,args) result (*name)args=dlsym(driver,#name);require(name!=NULL,#name)
    FN(FT_Error,FT_Init_FreeType,(FT_Library *));FN(FT_Error,FT_New_Memory_Face,(FT_Library,const FT_Byte *,FT_Long,FT_Long,FT_Face *));
    FN(FT_Error,FT_Set_Pixel_Sizes,(FT_Face,FT_UInt,FT_UInt));FN(FT_Error,FT_Load_Char,(FT_Face,FT_ULong,FT_Int32));
    FN(FT_UInt,FT_Get_Char_Index,(FT_Face,FT_ULong));FN(FT_Error,FT_Get_Kerning,(FT_Face,FT_UInt,FT_UInt,FT_UInt,FT_Vector *));
    FN(FT_Error,FT_Done_Face,(FT_Face));FN(FT_Error,FT_Done_FreeType,(FT_Library));
    FILE *file=fopen(argv[2],"rb");require(file!=NULL,"open system font fixture");require(fseek(file,0,SEEK_END)==0,"font seek");long size=ftell(file);require(size>0&&fseek(file,0,SEEK_SET)==0,"font size");
    unsigned char *bytes=malloc((size_t)size);require(bytes&&fread(bytes,1,(size_t)size,file)==(size_t)size,"font bytes");fclose(file);
    FT_Library library=NULL;FT_Face face=NULL;require(FT_Init_FreeType(&library)==0&&FT_New_Memory_Face(library,bytes,size,0,&face)==0,"initialize real font parser");
    require(face&&face->num_glyphs>100&&face->family_name&&FT_Set_Pixel_Sizes(face,0,28)==0,"font face and pixel size");
    const FT_ULong characters[]={'A',0x416,0x3a9};
    for(unsigned i=0;i<sizeof(characters)/sizeof(characters[0]);i++) {
        require(FT_Get_Char_Index(face,characters[i])!=0&&FT_Load_Char(face,characters[i],FT_LOAD_RENDER)==0,"map Unicode and rasterize glyph");
        FT_GlyphSlot glyph=face->glyph;require(glyph&&glyph->bitmap.width&&glyph->bitmap.rows&&glyph->bitmap.buffer&&glyph->bitmap.pixel_mode==FT_PIXEL_MODE_GRAY&&glyph->advance.x>0,"public glyph/bitmap ABI");
        uint64_t sum=0;for(unsigned y=0;y<glyph->bitmap.rows;y++)for(unsigned x=0;x<glyph->bitmap.width;x++)sum+=glyph->bitmap.buffer[(ptrdiff_t)y*glyph->bitmap.pitch+x];
        require(sum>0,"actual nonempty glyph raster");printf("FreeType U+%04lx bitmap=%ux%u coverage=%llu\n",characters[i],glyph->bitmap.width,glyph->bitmap.rows,(unsigned long long)sum);
    }
    FT_Vector kerning={0};require(FT_Get_Kerning(face,FT_Get_Char_Index(face,'A'),FT_Get_Char_Index(face,'V'),FT_KERNING_DEFAULT,&kerning)==0&&kerning.x<0,"font kerning");
    require(FT_Done_Face(face)==0&&FT_Done_FreeType(library)==0,"font parser teardown");free(bytes);require(dlclose(driver)==0,"close font driver");
    puts("PASS real x86 FreeType: Unicode glyph bitmaps, metrics, kerning and clean shutdown");return 0;
}
