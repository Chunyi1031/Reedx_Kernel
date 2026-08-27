#include <drives/drive_path.h>
#include <print.h>

int ParseDevicePath(EFI_DEVICE_PATH_PROTOCOL *path, device_path_info_t *info) {
    if (!path || !info) return -1;
    
    memset(info, 0, sizeof(device_path_info_t));
    
    EFI_DEVICE_PATH_PROTOCOL *dp = path;
    
    while (!IS_END_ENTIRE(dp)) {
        uint8_t type = dp->Type;
        uint8_t subtype = dp->SubType;
        uint32_t len = DEVICE_PATH_LENGTH(dp);

        
        switch (type) {
            case DEVICE_PATH_TYPE_HARDWARE:
                if (subtype == HARDWARE_SUBTYPE_PCI) {
                    PCI_DEVICE_PATH *pci = (PCI_DEVICE_PATH*)dp;
                    info->pci_device = pci->Device;
                    info->pci_function = pci->Function;
                    info->found_pci = 1;
                }
                break;
                
            case DEVICE_PATH_TYPE_MESSAGING:
                if (subtype == MESSAGING_SUBTYPE_ATAPI) {
                    ATAPI_DEVICE_PATH *atapi = (ATAPI_DEVICE_PATH*)dp;
                    info->ata_channel = atapi->PrimarySecondary;
                    info->ata_slave = atapi->SlaveMaster;
                    info->found_ata = 1;
                } else if (subtype == MESSAGING_SUBTYPE_SATA) {
                    SATA_DEVICE_PATH *sata = (SATA_DEVICE_PATH*)dp;
                    info->sata_port = sata->HBAPortNumber;
                    info->found_sata = 1;
                } else if (subtype == MESSAGING_SUBTYPE_NVME) {
                    NVME_DEVICE_PATH *nvme = (NVME_DEVICE_PATH*)dp;
                    info->nvme_namespace = nvme->NamespaceId;
                    info->found_nvme = 1;
                }
                break;
                
            case DEVICE_PATH_TYPE_MEDIA:
                if (subtype == MEDIA_SUBTYPE_HARDDRIVE) {
                    HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)dp;
                    info->partition_number = hd->PartitionNumber;
                    info->partition_start_lba = hd->PartitionStart;
                    info->partition_size_lba = hd->PartitionSize;
                    info->partition_mbr_type = hd->MBRType;
                    info->partition_signature_type = hd->SignatureType;
                    memcpy(info->partition_signature, hd->Signature, 16);
                    info->found_partition = 1;
                } else if (subtype == MEDIA_SUBTYPE_FILEPATH) {
                    FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH*)dp;
                    // 复制 UTF-16 字符串（简化：只转换 ASCII）
                    int i = 0;
                    while (fp->PathName[i] && i < 255) {
                        info->filepath[i] = (char)fp->PathName[i];  // 仅 ASCII
                        i++;
                    }
                    info->filepath[i] = '\0';
                    info->found_filepath = 1;
                }
                break;
        }
        
        // 跳到下一个节点
        dp = NEXT_DEVICE_PATH(dp);
    }
    
    return 0;
}