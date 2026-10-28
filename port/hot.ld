/* Section ordering (ld --section-ordering-file, see the Makefile): the
   renderer's inner loops first and together, 4052 bytes, so they can't
   evict each other in the direct-mapped fetch L1 (4 KB when this was
   written, 8 KB now). The groups are
   the rendering phases: walls (column, seg loop), floors and ceilings
   (planes, spans), sprites (masked and fuzz columns, which call
   R_DrawColumn through colfunc). Sizes from nm; if the set grows past
   the cache size, the functions at the end overlap the ones at the start. */
.text : {
    *(.text.R_DrawColumn)
    *(.text.R_GetColumn)
    *(.text.R_RenderSegLoop)
    *(.text.R_DrawSpan)
    *(.text.R_MapPlane)
    *(.text.R_MakeSpans)
    *(.text.R_DrawMaskedColumn)
    *(.text.R_DrawFuzzColumn)
    *(.text.FixedMul)
    *(.text.FixedDiv)
    *(.text.R_DrawPlanes)
}
