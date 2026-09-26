/* The FreeType modules xyuOS is built with, in place of the stock list in
 * freetype/config/ftmodule.h. The Makefile points FT_CONFIG_MODULES_H here.
 *
 * Only what reads the fonts that are actually on the disk: TrueType outlines
 * (Noto) and CFF ones (the CJK face is an .otf), with the PostScript helpers
 * CFF leans on, the sfnt container both come in, the anti-aliasing
 * rasteriser, and the auto-hinter. No Type 1, no bitmap-font formats, no
 * monochrome rasteriser: there is nothing on the system that would use them,
 * and each is code to carry for no one.
 */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
