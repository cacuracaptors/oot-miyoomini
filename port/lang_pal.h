/* gerado por tools/oot-idioma-pal.py (nao editar): posicoes das texturas/textos alemaes e franceses da ROM
 * europeia (PAL 1.1) que entram no lugar do ingles do cartucho americano (port/lang_pal.c). */
#define LANG_PAL_DMADATA 0x7950u
#define LANG_PAL_F_CODE 28
#define LANG_PAL_F_GER_MESSAGE_DATA_STATIC 22
#define LANG_PAL_F_FRA_MESSAGE_DATA_STATIC 23
typedef struct {
    uint16_t nf, pf;  /* arquivo (indice na dmadata): americano, europeu */
    uint32_t noff, len;
    int32_t poff[2];  /* no europeu: alemao, frances (-1 = nao tem: fica o ingles) */
} LangPalCopy;
static const LangPalCopy k_langPalCopy[] = {
    { 15, 15, 0x1EC00, 0x1EC00, { 125952, 251904 } }, /* item_name_static (bloco) */
    { 17, 17, 0x2B80, 0x2B80, { 11136, 22272 } }, /* do_action_static (bloco) */
    { 880, 881, 0xD80, 0xD80, { 3456, 6912 } }, /* gForestTempleTitleCardENGTex */
    { 881, 882, 0xD80, 0xD80, { 3456, 6912 } }, /* gShadowTempleTitleCardENGTex */
    { 882, 883, 0xD80, 0xD80, { 3456, 6912 } }, /* gFireTempleTitleCardENGTex */
    { 883, 884, 0xD80, 0xD80, { 3456, 6912 } }, /* gWaterTempleTitleCardENGTex */
    { 884, 885, 0xD80, 0xD80, { 3456, 6912 } }, /* gSpiritTempleTitleCardENGTex */
    { 885, 886, 0xD80, 0xD80, { 3456, 6912 } }, /* gDekuTreeTitleCardENGTex */
    { 886, 887, 0xD80, 0xD80, { 3456, 6912 } }, /* gJabuJabuTitleCardENGTex */
    { 887, 888, 0xD80, 0xD80, { 3456, 6912 } }, /* gDodongosCavernTitleCardENGTex */
    { 888, 889, 0xD80, 0xD80, { 3456, 6912 } }, /* gInsideGanonsCastleTitleCardENGTex */
    { 889, 890, 0xD80, 0xD80, { 3456, 6912 } }, /* gIceCavernTitleCardENGTex */
    { 890, 891, 0xD80, 0xD80, { 3456, 6912 } }, /* gGERudoTrainingGroundTitleCardENGTex */
    { 891, 892, 0xD80, 0xD80, { 3456, 6912 } }, /* gCastleCourtyardTitleCardENGTex */
    { 892, 893, 0xD80, 0xD80, { 3456, 6912 } }, /* gGreatFairysFountainTitleCardENGTex */
    { 893, 894, 0xD80, 0xD80, { 3456, 6912 } }, /* gChamberOfTheSagesTitleCardENGTex */
    { 894, 895, 0xD80, 0xD80, { 3456, 6912 } }, /* gShootingGalleryTitleCardENGTex */
    { 895, 896, 0xD80, 0xD80, { 3456, 6912 } }, /* gTempleOfTimeTitleCardENGTex */
    { 896, 897, 0xD80, 0xD80, { 3456, 6912 } }, /* gMarketTitleCardENGTex */
    { 897, 898, 0xD80, 0xD80, { 3456, 6912 } }, /* gBackAlleyTitleCardENGTex */
    { 898, 899, 0xD80, 0xD80, { 3456, 6912 } }, /* gKokiriShopTitleCardENGTex */
    { 899, 900, 0xD80, 0xD80, { 3456, 6912 } }, /* gGoronShopTitleCardENGTex */
    { 900, 901, 0xD80, 0xD80, { 3456, 6912 } }, /* gZoraShopTitleCardENGTex */
    { 901, 902, 0xD80, 0xD80, { 3456, 6912 } }, /* gHouseOfSkulltulaTitleCardENGTex */
    { 902, 903, 0xD80, 0xD80, { 3456, 6912 } }, /* gBazaarTitleCardENGTex */
    { 903, 904, 0xD80, 0xD80, { 3456, 6912 } }, /* gPotionShopTitleCardENGTex */
    { 904, 905, 0xD80, 0xD80, { 3456, 6912 } }, /* gGravekeepersHutTitleCardENGTex */
    { 905, 906, 0xD80, 0xD80, { 3456, 6912 } }, /* gLakesideLaboratoryTitleCardENGTex */
    { 906, 907, 0xD80, 0xD80, { 3456, 6912 } }, /* gHyruleFieldTitleCardENGTex */
    { 907, 908, 0xD80, 0xD80, { 3456, 6912 } }, /* gKakarikoVillageTitleCardENGTex */
    { 908, 909, 0xD80, 0xD80, { 3456, 6912 } }, /* gGraveyardTitleCardENGTex */
    { 909, 910, 0xD80, 0xD80, { 3456, 6912 } }, /* gZorasRiverTitleCardENGTex */
    { 910, 911, 0xD80, 0xD80, { 3456, 6912 } }, /* gKokiriForestTitleCardENGTex */
    { 911, 912, 0xD80, 0xD80, { 3456, 6912 } }, /* gLakeHyliaTitleCardENGTex */
    { 912, 913, 0xD80, 0xD80, { 3456, 6912 } }, /* gZorasDomainTitleCardENGTex */
    { 913, 914, 0xD80, 0xD80, { 3456, 6912 } }, /* gZorasFountainTitleCardENGTex */
    { 914, 915, 0xD80, 0xD80, { 3456, 6912 } }, /* gGERudoValleyTitleCardENGTex */
    { 915, 916, 0xD80, 0xD80, { 3456, 6912 } }, /* gLostWoodsTitleCardENGTex */
    { 916, 917, 0xD80, 0xD80, { 3456, 6912 } }, /* gHauntedWastelandTitleCardENGTex */
    { 917, 918, 0xD80, 0xD80, { 3456, 6912 } }, /* gHyruleCastleTitleCardENGTex */
    { 918, 919, 0xD80, 0xD80, { 3456, 6912 } }, /* gDeathMountainTrailTitleCardENGTex */
    { 919, 920, 0xD80, 0xD80, { 3456, 6912 } }, /* gDeathMountainCraterTitleCardENGTex */
    { 920, 921, 0xD80, 0xD80, { 3456, 6912 } }, /* gGoronCityTitleCardENGTex */
    { 921, 922, 0xD80, 0xD80, { 3456, 6912 } }, /* gLonLonRanchTitleCardENGTex */
    { 922, 923, 0xD80, 0xD80, { 3456, 6912 } }, /* gGanonsCastleTitleCardENGTex */
    { 923, 924, 0xD80, 0xD80, { 3456, 6912 } }, /* gRoyalFamilysTombTitleCardENGTex */
    { 924, 925, 0xD80, 0xD80, { 3456, 6912 } }, /* gFairysFountainTitleCardENGTex */
    { 925, 926, 0xD80, 0xD80, { 3456, 6912 } }, /* gFishingPondTitleCardENGTex */
    { 926, 927, 0xD80, 0xD80, { 3456, 6912 } }, /* gBombchuBowlingAlleyCardENGTex */
    { 927, 928, 0xD80, 0xD80, { 3456, 6912 } }, /* gStableTitleCardENGTex */
    { 928, 929, 0xD80, 0xD80, { 3456, 6912 } }, /* gThievesHideoutTitleCardENGTex */
    { 929, 930, 0xD80, 0xD80, { 3456, 6912 } }, /* gHappyMaskShopTitleCardENGTex */
    { 930, 931, 0xD80, 0xD80, { 3456, 6912 } }, /* gTreasureBoxShopTitleCardENGTex */
    { 931, 932, 0xD80, 0xD80, { 3456, 6912 } }, /* gSacredForestMeadowTitleCardENGTex */
    { 932, 933, 0xD80, 0xD80, { 3456, 6912 } }, /* gGERudosFortressTitleCardENGTex */
    { 933, 934, 0xD80, 0xD80, { 3456, 6912 } }, /* gBottomOfTheWellTitleCardENGTex */
    { 934, 935, 0xD80, 0xD80, { 3456, 6912 } }, /* gDesertColossusTitleCardENGTex */
    { 935, 936, 0xD80, 0xD80, { 3456, 6912 } }, /* gBombchuShopTitleCardENGTex */
    { 936, 937, 0xD80, 0xD80, { 3456, 6912 } }, /* gQuestionMarkTitleCardENGTex */
    { 12, 11, 0x2980, 0x980, { 10624, 13056 } }, /* gContinuePlayingENGTex */
    { 14, 13, 0x0, 0x600, { 0, -1 } }, /* gPauseDekuTitleENGTex */
    { 14, 14, 0x0, 0x600, { -1, 0 } }, /* gPauseDekuTitleENGTex */
    { 14, 13, 0x600, 0x600, { 1536, -1 } }, /* gPauseDodongoTitleENGTex */
    { 14, 14, 0x600, 0x600, { -1, 1536 } }, /* gPauseDodongoTitleENGTex */
    { 14, 13, 0xC00, 0x600, { 3072, -1 } }, /* gPauseJabuTitleENGTex */
    { 14, 14, 0xC00, 0x600, { -1, 3072 } }, /* gPauseJabuTitleENGTex */
    { 14, 13, 0x1200, 0x600, { 4608, -1 } }, /* gPauseForestTitleENGTex */
    { 14, 14, 0x1200, 0x600, { -1, 4608 } }, /* gPauseForestTitleENGTex */
    { 14, 13, 0x1800, 0x600, { 6144, -1 } }, /* gPauseFireTitleENGTex */
    { 14, 14, 0x1800, 0x600, { -1, 6144 } }, /* gPauseFireTitleENGTex */
    { 14, 13, 0x1E00, 0x600, { 7680, -1 } }, /* gPauseWaterTitleENGTex */
    { 14, 14, 0x1E00, 0x600, { -1, 7680 } }, /* gPauseWaterTitleENGTex */
    { 14, 13, 0x2400, 0x600, { 9216, -1 } }, /* gPauseSpiritTitleENGTex */
    { 14, 14, 0x2400, 0x600, { -1, 9216 } }, /* gPauseSpiritTitleENGTex */
    { 14, 13, 0x2A00, 0x600, { 10752, -1 } }, /* gPauseShadowTitleENGTex */
    { 14, 14, 0x2A00, 0x600, { -1, 10752 } }, /* gPauseShadowTitleENGTex */
    { 14, 13, 0x3000, 0x600, { 12288, -1 } }, /* gPauseBotWTitleENGTex */
    { 14, 14, 0x3000, 0x600, { -1, 12288 } }, /* gPauseBotWTitleENGTex */
    { 14, 13, 0x3600, 0x600, { 13824, -1 } }, /* gPauseIceCavernTitleENGTex */
    { 14, 14, 0x3600, 0x600, { -1, 13824 } }, /* gPauseIceCavernTitleENGTex */
    { 14, 13, 0x4880, 0x800, { 19840, -1 } }, /* gPauseToSelectItemENGTex */
    { 14, 14, 0x4880, 0x800, { -1, 19584 } }, /* gPauseToSelectItemENGTex */
    { 14, 13, 0x5080, 0x800, { 21888, -1 } }, /* gPauseToMapENGTex */
    { 14, 14, 0x5080, 0x800, { -1, 21632 } }, /* gPauseToMapENGTex */
    { 14, 13, 0x5880, 0x800, { 23936, -1 } }, /* gPauseToQuestStatusENGTex */
    { 14, 14, 0x5880, 0x800, { -1, 23680 } }, /* gPauseToQuestStatusENGTex */
    { 14, 13, 0x6080, 0x800, { 25984, -1 } }, /* gPauseToEquipmentENGTex */
    { 14, 14, 0x6080, 0x800, { -1, 25728 } }, /* gPauseToEquipmentENGTex */
    { 14, 13, 0x6880, 0x980, { 28032, -1 } }, /* gPauseSavePromptENGTex */
    { 14, 14, 0x6880, 0x980, { -1, 27776 } }, /* gPauseSavePromptENGTex */
    { 14, 13, 0x7200, 0x980, { 30464, -1 } }, /* gPauseSaveConfirmationENGTex */
    { 14, 14, 0x7200, 0x980, { -1, 30208 } }, /* gPauseSaveConfirmationENGTex */
    { 14, 13, 0x7B80, 0x300, { 32896, -1 } }, /* gPauseYesENGTex */
    { 14, 14, 0x7B80, 0x300, { -1, 32640 } }, /* gPauseYesENGTex */
    { 14, 13, 0x7E80, 0x300, { 33664, -1 } }, /* gPauseNoENGTex */
    { 14, 14, 0x7E80, 0x300, { -1, 33408 } }, /* gPauseNoENGTex */
    { 14, 13, 0x8180, 0x100, { 34432, -1 } }, /* gPauseCurrentPositionENGTex */
    { 14, 14, 0x8180, 0x100, { -1, 34176 } }, /* gPauseCurrentPositionENGTex */
    { 14, 13, 0x8280, 0xA00, { 37248, -1 } }, /* gPauseEquipment10ENGTex */
    { 14, 14, 0x8280, 0xA00, { -1, 36992 } }, /* gPauseEquipment10ENGTex */
    { 14, 13, 0x8C80, 0xA00, { 42368, -1 } }, /* gPauseSelectItem00ENGTex */
    { 14, 14, 0x8C80, 0xA00, { -1, 42112 } }, /* gPauseSelectItem00ENGTex */
    { 14, 13, 0x9680, 0xA00, { 44928, -1 } }, /* gPauseSelectItem10ENGTex */
    { 14, 14, 0x9680, 0xA00, { -1, 44672 } }, /* gPauseSelectItem10ENGTex */
    { 14, 13, 0xA080, 0xA00, { 47488, -1 } }, /* gPauseSelectItem20ENGTex */
    { 14, 14, 0xA080, 0xA00, { -1, 47232 } }, /* gPauseSelectItem20ENGTex */
    { 14, 13, 0xAA80, 0xA00, { 50048, -1 } }, /* gPauseMap10ENGTex */
    { 14, 14, 0xAA80, 0xA00, { -1, 49792 } }, /* gPauseMap10ENGTex */
    { 14, 13, 0xBE80, 0xA00, { 52608, -1 } }, /* gPauseQuestStatus10ENGTex */
    { 14, 14, 0xBE80, 0xA00, { -1, 52352 } }, /* gPauseQuestStatus10ENGTex */
    { 14, 13, 0xD280, 0xA00, { 55168, -1 } }, /* gPauseSave10ENGTex */
    { 14, 14, 0xD280, 0xA00, { -1, 57472 } }, /* gPauseSave10ENGTex */
    { 16, 16, 0x3000, 0x400, { 12288, 24576 } }, /* gHauntedWastelandPointNameENGTex */
    { 16, 16, 0x3400, 0x400, { 13312, 25600 } }, /* gGerudosFortressPointNameENGTex */
    { 16, 16, 0x3800, 0x400, { 14336, 26624 } }, /* gGerudoValleyPointNameENGTex */
    { 16, 16, 0x3C00, 0x400, { 15360, 27648 } }, /* gHyliaLakesidePointNameENGTex */
    { 16, 16, 0x4000, 0x400, { 16384, 28672 } }, /* gLonLonRanchPointNameENGTex */
    { 16, 16, 0x4400, 0x400, { 17408, 29696 } }, /* gMarketPointNameENGTex */
    { 16, 16, 0x4800, 0x400, { 18432, 30720 } }, /* gHyruleFieldPointNameENGTex */
    { 16, 16, 0x4C00, 0x400, { 19456, 31744 } }, /* gDeathMountainPointNameENGTex */
    { 16, 16, 0x5000, 0x400, { 20480, 32768 } }, /* gKakarikoVillagePointNameENGTex */
    { 16, 16, 0x5400, 0x400, { 21504, 33792 } }, /* gLostWoodsPointNameENGTex */
    { 16, 16, 0x5800, 0x400, { 22528, 34816 } }, /* gKokiriForestPointNameENGTex */
    { 16, 16, 0x5C00, 0x400, { 23552, 35840 } }, /* gZorasDomainPointNameENGTex */
    { 16, 16, 0x13C00, 0xA00, { 93184, 149504 } }, /* gHyruleFieldPositionNameENGTex */
    { 16, 16, 0x14600, 0xA00, { 95744, 152064 } }, /* gKakarikoVillagePositionNameENGTex */
    { 16, 16, 0x15000, 0xA00, { 98304, 154624 } }, /* gGraveyardPositionNameENGTex */
    { 16, 16, 0x15A00, 0xA00, { 100864, 157184 } }, /* gZorasRiverPositionNameENGTex */
    { 16, 16, 0x16400, 0xA00, { 103424, 159744 } }, /* gKokiriForestPositionNameENGTex */
    { 16, 16, 0x16E00, 0xA00, { 105984, 162304 } }, /* gSacredForestMeadowPositionNameENGTex */
    { 16, 16, 0x17800, 0xA00, { 108544, 164864 } }, /* gLakeHyliaPositionNameENGTex */
    { 16, 16, 0x18200, 0xA00, { 111104, 167424 } }, /* gZorasDomainPositionNameENGTex */
    { 16, 16, 0x18C00, 0xA00, { 113664, 169984 } }, /* gZorasFountainPositionNameENGTex */
    { 16, 16, 0x19600, 0xA00, { 116224, 172544 } }, /* gGerudoValleyPositionNameENGTex */
    { 16, 16, 0x1A000, 0xA00, { 118784, 175104 } }, /* gLostWoodsPositionNameENGTex */
    { 16, 16, 0x1AA00, 0xA00, { 121344, 177664 } }, /* gDesertColossusPositionNameENGTex */
    { 16, 16, 0x1B400, 0xA00, { 123904, 180224 } }, /* gGerudosFortressPositionNameENGTex */
    { 16, 16, 0x1BE00, 0xA00, { 126464, 182784 } }, /* gHauntedWastelandPositionNameENGTex */
    { 16, 16, 0x1C800, 0xA00, { 129024, 185344 } }, /* gMarketPositionNameENGTex */
    { 16, 16, 0x1D200, 0xA00, { 131584, 187904 } }, /* gHyruleCastlePositionNameENGTex */
    { 16, 16, 0x1DC00, 0xA00, { 134144, 190464 } }, /* gDeathMountainTrailPositionNameENGTex */
    { 16, 16, 0x1E600, 0xA00, { 136704, 193024 } }, /* gDeathMountainCraterPositionNameENGTex */
    { 16, 16, 0x1F000, 0xA00, { 139264, 195584 } }, /* gGoronCityPositionNameENGTex */
    { 16, 16, 0x1FA00, 0xA00, { 141824, 198144 } }, /* gLonLonRanchPositionNameENGTex */
    { 16, 16, 0x20400, 0xA00, { 144384, 200704 } }, /* gQuestionMarkPositionNameENGTex */
    { 16, 16, 0x20E00, 0xA00, { 146944, 203264 } }, /* gGanonsCastlePositionNameENGTex */
    { 620, 621, 0x2630, 0x1400, { 9776, 14896 } }, /* gBarinadeTitleCardTex */
    { 637, 638, 0xEB00, 0x1400, { 60160, 65280 } }, /* gVolvagiaTitleCardTex */
    { 582, 583, 0x6DA0, 0x1400, { 28064, 33184 } }, /* gPhantomGanonTitleCardTex */
    { 705, 706, 0xE300, 0x1400, { 58112, 63232 } }, /* gGanondorfTitleCardTex */
    { 814, 815, 0x22E90, 0x1400, { 142992, 148112 } }, /* gGanonTitleCardTex */
    { 520, 521, 0x1AFA8, 0x1400, { 110504, 115624 } }, /* gGohmaTitleCardTex */
    { 517, 518, 0x18810, 0x1400, { 100368, 105488 } }, /* gKingDodongoTitleCardTex */
    { 660, 661, 0x2410, 0x1400, { 9232, 14352 } }, /* gMorphaTitleCardTex */
    { 706, 707, 0x15180, 0x1400, { 86400, 91520 } }, /* gBongoTitleCardTex */
    { 691, 692, 0x2F570, 0x1400, { 193904, 199024 } }, /* gTwinrovaTitleCardTex */
    { 939, 940, 0x8C00, 0x400, { -1, 6784 } }, /* gFileSelSOUNDENGTex */
    { 939, 940, 0x21680, 0x580, { 112768, 114176 } }, /* gFileSelENDButtonENGTex */
    { 939, 940, 0x2AF00, 0x800, { 201216, 249472 } }, /* gFileSelNoFileToCopyENGTex */
    { 939, 940, 0x2B700, 0x800, { 203264, 251520 } }, /* gFileSelNoFileToEraseENGTex */
    { 939, 940, 0x2BF00, 0x800, { 205312, 253568 } }, /* gFileSelNoEmptyFileENGTex */
    { 939, 940, 0x2C700, 0x800, { 207360, 255616 } }, /* gFileSelFileEmptyENGTex */
    { 939, 940, 0x2CF00, 0x800, { 209408, 257664 } }, /* gFileSelFileInUseENGTex */
    { 939, 940, 0x2D700, 0x800, { 211456, 259712 } }, /* gFileSelCopyWhichFileENGTex */
    { 939, 940, 0x2DF00, 0x800, { 213504, 261760 } }, /* gFileSelCopyToWhichFileENGTex */
    { 939, 940, 0x2E700, 0x800, { 215552, 263808 } }, /* gFileSelAreYouSureENGTex */
    { 939, 940, 0x2EF00, 0x800, { 217600, 265856 } }, /* gFileSelFileCopiedENGTex */
    { 939, 940, 0x2F700, 0x800, { 219648, 267904 } }, /* gFileSelPleaseSelectAFileENGTex */
    { 939, 940, 0x2FF00, 0x800, { 221696, 269952 } }, /* gFileSelOpenThisFileENGTex */
    { 939, 940, 0x30700, 0x800, { 223744, 272000 } }, /* gFileSelEraseWhichFileENGTex */
    { 939, 940, 0x30F00, 0x800, { 225792, 274048 } }, /* gFileSelAreYouSure2ENGTex */
    { 939, 940, 0x31700, 0x800, { 227840, 276096 } }, /* gFileSelFileErasedENGTex */
    { 939, 940, 0x31F00, 0x800, { 229888, -1 } }, /* gFileSelOptionsENGTex */
    { 939, 940, 0x32700, 0x380, { 231936, 278144 } }, /* gFileSelNameENGTex */
    { 939, 940, 0x32D80, 0x300, { 3456, 4224 } }, /* gFileSelHeadsetENGTex */
    { 939, 940, 0x33380, 0x300, { -1, 8576 } }, /* gFileSelStereoENGTex */
    { 939, 940, 0x33680, 0x400, { -1, 12672 } }, /* gFileSelZTargetingENGTex */
    { 939, 940, 0x33A80, 0x300, { -1, 15744 } }, /* gFileSelSwitchENGTex */
    { 939, 940, 0x33D80, 0x300, { -1, 18560 } }, /* gFileSelHoldENGTex */
    { 939, 940, 0x34080, 0x900, { 232832, 279040 } }, /* gFileSelControlsENGTex */
    { 939, 940, 0x34F80, 0x800, { 235136, 281344 } }, /* gFileSelCopyButtonENGTex */
    { 939, 940, 0x35780, 0x800, { 237184, 283392 } }, /* gFileSelFile1ButtonENGTex */
    { 939, 940, 0x35F80, 0x800, { 239232, 285440 } }, /* gFileSelFile2ButtonENGTex */
    { 939, 940, 0x36780, 0x800, { 241280, 287488 } }, /* gFileSelFile3ButtonENGTex */
    { 939, 940, 0x36F80, 0x800, { 243328, 289536 } }, /* gFileSelYesButtonENGTex */
    { 939, 940, 0x37780, 0x800, { 245376, 291584 } }, /* gFileSelEraseButtonENGTex */
    { 939, 940, 0x37F80, 0x800, { 197120, -1 } }, /* gFileSelOptionsButtonENGTex */
    { 939, 940, 0x38780, 0x800, { 247424, 293632 } }, /* gFileSelQuitButtonENGTex */
    { 21, 20, 0x0, 0x4580, { 0, 0 } }, /* fonte */
};
/* rotulos do menu de pausa mais largos que o ingles (ia8, 16 de altura): Port_LangExtraTex(0..2) */
typedef struct {
    uint16_t pf[2];
    uint32_t poff[2];
    uint16_t w[2];
} LangPalExtra;
static const LangPalExtra k_langPalExtra[] = {
    { { 13, 14 }, { 0x3C00, 0x3C00 }, { 88, 80 } }, /* gPauseToEquip */
    { { 13, 14 }, { 0x4180, 0x4100 }, { 88, 72 } }, /* gPauseToDecide */
    { { 13, 14 }, { 0x4700, 0x4580 }, { 104, 112 } }, /* gPauseToPlayMelody */
};
