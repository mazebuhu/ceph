import { Component, OnInit } from '@angular/core';
import { FormControl, FormGroup, Validators } from '@angular/forms';
import { ActivatedRoute, Router } from '@angular/router';

import * as _ from 'lodash';

import { PoolService } from '../../../shared/api/pool.service';
import { RbdService } from '../../../shared/api/rbd.service';
import { NotificationType } from '../../../shared/enum/notification-type.enum';
import { FinishedTask } from '../../../shared/models/finished-task';
import { DimlessBinaryPipe } from '../../../shared/pipes/dimless-binary.pipe';
import { FormatterService } from '../../../shared/services/formatter.service';
import { NotificationService } from '../../../shared/services/notification.service';
import { TaskManagerMessageService } from '../../../shared/services/task-manager-message.service';
import { TaskManagerService } from '../../../shared/services/task-manager.service';
import { RbdFormCreateRequestModel } from './rbd-form-create-request.model';
import { RbdFormEditRequestModel } from './rbd-form-edit-request.model';
import { RbdFormResponseModel } from './rbd-form-response.model';

@Component({
  selector: 'cd-rbd-form',
  templateUrl: './rbd-form.component.html',
  styleUrls: ['./rbd-form.component.scss']
})
export class RbdFormComponent implements OnInit {

  rbdForm: FormGroup;
  featuresFormGroups: FormGroup;
  defaultFeaturesFormControl: FormControl;
  deepFlattenFormControl: FormControl;
  layeringFormControl: FormControl;
  exclusiveLockFormControl: FormControl;
  objectMapFormControl: FormControl;
  journalingFormControl: FormControl;
  fastDiffFormControl: FormControl;

  pools: Array<string> = null;
  allPools: Array<string> = null;
  dataPools: Array<string> = null;
  allDataPools: Array<string> = null;
  features: any;
  featuresList = [];

  routeParamsSubscribe: any;
  pool: string;

  advancedEnabled = false;

  editing = false;

  response: RbdFormResponseModel;

  defaultObjectSize = '4MiB';

  objectSizes: Array<string> = [
    '4KiB',
    '8KiB',
    '16KiB',
    '32KiB',
    '64KiB',
    '128KiB',
    '256KiB',
    '512KiB',
    '1MiB',
    '2MiB',
    '4MiB',
    '8MiB',
    '16MiB',
    '32MiB'
  ];

  constructor(private route: ActivatedRoute,
              private router: Router,
              private poolService: PoolService,
              private rbdService: RbdService,
              private formatter: FormatterService,
              private dimlessBinaryPipe: DimlessBinaryPipe,
              private taskManagerService: TaskManagerService,
              private taskManagerMessageService: TaskManagerMessageService,
              private notificationService: NotificationService) {
    this.features = {
      'deep-flatten': {
        desc: 'Deep flatten',
        requires: null,
        allowEnable: false,
        allowDisable: true
      },
      'layering': {
        desc: 'Layering',
        requires: null,
        allowEnable: false,
        allowDisable: false
      },
      'exclusive-lock': {
        desc: 'Exclusive lock',
        requires: null,
        allowEnable: true,
        allowDisable: true
      },
      'object-map': {
        desc: 'Object map (requires exclusive-lock)',
        requires: 'exclusive-lock',
        allowEnable: true,
        allowDisable: true
      },
      'journaling': {
        desc: 'Journaling (requires exclusive-lock)',
        requires: 'exclusive-lock',
        allowEnable: true,
        allowDisable: true
      },
      'fast-diff': {
        desc: 'Fast diff (requires object-map)',
        requires: 'object-map',
        allowEnable: true,
        allowDisable: true
      }
    };
    this.createForm();
    for (const key of Object.keys(this.features)) {
      const listItem = this.features[key];
      listItem.key = key;
      this.featuresList.push(listItem);
    }
  }

  createForm() {
    this.defaultFeaturesFormControl = new FormControl(true);
    this.deepFlattenFormControl = new FormControl(false);
    this.layeringFormControl = new FormControl(false);
    this.exclusiveLockFormControl = new FormControl(false);
    this.objectMapFormControl = new FormControl({value: false, disabled: true});
    this.journalingFormControl = new FormControl({value: false, disabled: true});
    this.fastDiffFormControl = new FormControl({value: false, disabled: true});
    this.featuresFormGroups = new FormGroup({
      defaultFeatures: this.defaultFeaturesFormControl,
      'deep-flatten': this.deepFlattenFormControl,
      'layering': this.layeringFormControl,
      'exclusive-lock': this.exclusiveLockFormControl,
      'object-map': this.objectMapFormControl,
      'journaling': this.journalingFormControl,
      'fast-diff': this.fastDiffFormControl,
    });
    this.rbdForm = new FormGroup({
      name: new FormControl('', {
        validators: [
          Validators.required
        ]
      }),
      pool: new FormControl(null, {
        validators: [
          Validators.required
        ]
      }),
      useDataPool: new FormControl(false),
      dataPool: new FormControl(null),
      size: new FormControl(null, {
        updateOn: 'blur'
      }),
      obj_size: new FormControl(this.defaultObjectSize),
      features: this.featuresFormGroups,
      stripingUnit: new FormControl(null),
      stripingCount: new FormControl(null, {
        updateOn: 'blur'
      })
    }, this.validateRbdForm(this.formatter));
  }

  disableForEdit() {
    this.rbdForm.get('pool').disable();
    this.rbdForm.get('useDataPool').disable();
    this.rbdForm.get('dataPool').disable();
    this.rbdForm.get('obj_size').disable();
    this.rbdForm.get('stripingUnit').disable();
    this.rbdForm.get('stripingCount').disable();
  }

  ngOnInit() {
    if (this.router.url.startsWith('/rbd/edit')) {
      this.editing = true;
    }
    if (this.editing) {
      this.disableForEdit();
      this.routeParamsSubscribe = this.route.params.subscribe(
        (params: { pool: string, name: string }) => {
          const poolName = params.pool;
          const rbdName = params.name;
          this.rbdService.get(poolName, rbdName)
            .subscribe((resp: RbdFormResponseModel) => {
              this.setResponse(resp);
            });
        }
      );
    }
    this.poolService.list(['pool_name', 'type', 'flags_names', 'application_metadata']).then(
      resp => {
        const pools = [];
        const dataPools = [];
        for (const pool of resp) {
          if (!_.isUndefined(pool.application_metadata.rbd)) {
            if (pool.type === 'replicated') {
              pools.push(pool);
              dataPools.push(pool);
            } else if (pool.type === 'erasure' &&
              pool.flags_names.indexOf('ec_overwrites') !== -1) {
              dataPools.push(pool);
            }
          }
        }
        this.pools = pools;
        this.allPools = pools;
        this.dataPools = dataPools;
        this.allDataPools = dataPools;
        if (this.pools.length === 1) {
          const poolName = this.pools[0]['pool_name'];
          this.rbdForm.get('pool').setValue(poolName);
          this.onPoolChange(poolName);
        }
      }
    );
    this.defaultFeaturesFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures(null, value);
    });
    this.deepFlattenFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('deep-flatten', value);
    });
    this.layeringFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('layering', value);
    });
    this.exclusiveLockFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('exclusive-lock', value);
    });
    this.objectMapFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('object-map', value);
    });
    this.journalingFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('journaling', value);
    });
    this.fastDiffFormControl.valueChanges.subscribe((value) => {
      this.watchDataFeatures('fast-diff', value);
    });
  }

  onPoolChange(selectedPoolName) {
    const newDataPools = this.allDataPools.filter((dataPool: any) => {
      return dataPool.pool_name !== selectedPoolName;
    });
    if (this.rbdForm.get('dataPool').value === selectedPoolName) {
      this.rbdForm.get('dataPool').setValue(null);
    }
    this.dataPools = newDataPools;
  }

  onUseDataPoolChange () {
    if (!this.rbdForm.get('useDataPool').value) {
      this.rbdForm.get('dataPool').setValue(null);
      this.onDataPoolChange(null);
    }
  }

  onDataPoolChange(selectedDataPoolName) {
    const newPools = this.allPools.filter((pool: any) => {
      return pool.pool_name !== selectedDataPoolName;
    });
    if (this.rbdForm.get('pool').value === selectedDataPoolName) {
      this.rbdForm.get('pool').setValue(null);
    }
    this.pools = newPools;
  }

  validateRbdForm(formatter: FormatterService) {
    return (formGroup: FormGroup) => {
      // Data Pool
      const useDataPoolControl = formGroup.get('useDataPool');
      const dataPoolControl = formGroup.get('dataPool');
      let dataPoolControlErrors = null;
      if (useDataPoolControl.value && dataPoolControl.value == null) {
        dataPoolControlErrors = {'required': true};
      }
      dataPoolControl.setErrors(dataPoolControlErrors);
      // Size
      const sizeControl = formGroup.get('size');
      const objectSizeControl = formGroup.get('obj_size');
      const objectSizeInBytes = formatter.toBytes(
        objectSizeControl.value != null ? objectSizeControl.value : this.defaultObjectSize);
      const stripingCountControl = formGroup.get('stripingCount');
      const stripingCount = stripingCountControl.value != null ? stripingCountControl.value : 1;
      let sizeControlErrors = null;
      if (sizeControl.value === null) {
        sizeControlErrors = {'required': true};
      } else {
        const sizeInBytes = formatter.toBytes(sizeControl.value);
        if (stripingCount * objectSizeInBytes > sizeInBytes) {
          sizeControlErrors = {'invalidSizeObject': true};
        }
      }
      sizeControl.setErrors(sizeControlErrors);
      // Striping Unit
      const stripingUnitControl = formGroup.get('stripingUnit');
      let stripingUnitControlErrors = null;
      if (stripingUnitControl.value === null && stripingCountControl.value !== null) {
        stripingUnitControlErrors = {'required': true};
      } else if (stripingUnitControl.value !== null) {
        const stripingUnitInBytes = formatter.toBytes(stripingUnitControl.value);
        if (stripingUnitInBytes > objectSizeInBytes) {
          stripingUnitControlErrors = {'invalidStripingUnit': true};
        }
      }
      stripingUnitControl.setErrors(stripingUnitControlErrors);
      // Striping Count
      let stripingCountControlErrors = null;
      if (stripingCountControl.value === null && stripingUnitControl.value !== null) {
        stripingCountControlErrors = {'required': true};
      } else if (stripingCount < 1) {
        stripingCountControlErrors = {'min': true};
      }
      stripingCountControl.setErrors(stripingCountControlErrors);
    };
  }

  deepBoxCheck(key, checked) {
    _.forIn(this.features, (details, feature) => {
      if (details.requires === key) {
        if (checked) {
          this.featuresFormGroups.get(feature).enable();
        } else {
          this.featuresFormGroups.get(feature).disable();
          this.featuresFormGroups.get(feature).setValue(checked);
          this.watchDataFeatures(feature, checked);
          this.deepBoxCheck(feature, checked);
        }
      }
      if (this.editing && this.featuresFormGroups.get(feature).enabled) {

        if (this.response.features_name.indexOf(feature) !== -1 && !details.allowDisable) {
          this.featuresFormGroups.get(feature).disable();

        } else if (this.response.features_name.indexOf(feature) === -1 && !details.allowEnable) {
          this.featuresFormGroups.get(feature).disable();
        }
      }
    });
  }

  featureFormUpdate(key, checked) {
    if (checked) {
      const required = this.features[key].requires;
      if (required && !this.featuresFormGroups.get(required).value) {
        this.featuresFormGroups.get(key).setValue(false);
        return;
      }
    }
    this.deepBoxCheck(key, checked);
  }

  watchDataFeatures(key, checked) {
    if (!this.defaultFeaturesFormControl.value && key) {
      this.featureFormUpdate(key, checked);
    }
  }

  setResponse(response: RbdFormResponseModel) {
    this.response = response;
    this.rbdForm.get('name').setValue(response.name);
    this.rbdForm.get('pool').setValue(response.pool_name);
    if (response.data_pool) {
      this.rbdForm.get('useDataPool').setValue(true);
      this.rbdForm.get('dataPool').setValue(response.data_pool);
    }
    this.rbdForm.get('size').setValue(this.dimlessBinaryPipe.transform(response.size));
    this.rbdForm.get('obj_size').setValue(this.dimlessBinaryPipe.transform(response.obj_size));
    const featuresControl = this.rbdForm.get('features');
    featuresControl.get('defaultFeatures').setValue(false);
    _.forIn(this.features, (feature) => {
      if (response.features_name.indexOf(feature.key) !== -1) {
        featuresControl.get(feature.key).setValue(true);
      }
    });
    this.rbdForm.get('stripingUnit').setValue(
      this.dimlessBinaryPipe.transform(response.stripe_unit));
    this.rbdForm.get('stripingCount').setValue(response.stripe_count);
  }

  createRequest() {
    const request = new RbdFormCreateRequestModel();
    request.pool_name = this.rbdForm.get('pool').value;
    request.name = this.rbdForm.get('name').value;
    request.size = this.formatter.toBytes(this.rbdForm.get('size').value);
    request.obj_size = this.formatter.toBytes(this.rbdForm.get('obj_size').value);
    if (!this.defaultFeaturesFormControl.value) {
      _.forIn(this.features, (feature) => {
        if (this.featuresFormGroups.get(feature.key).value) {
          request.features.push(feature.key);
        }
      });
    } else {
      request.features = null;
    }
    request.stripe_unit = this.formatter.toBytes(this.rbdForm.get('stripingUnit').value);
    request.stripe_count = this.rbdForm.get('stripingCount').value;
    request.data_pool = this.rbdForm.get('dataPool').value;
    return request;
  }

  createAction() {
    const request = this.createRequest();
    const finishedTask = new FinishedTask();
    finishedTask.name = 'rbd/create';
    finishedTask.metadata = {'pool_name': request.pool_name, 'image_name': request.name};
    this.rbdService.create(request).toPromise().then((resp) => {
      if (resp.status === 202) {
        this.notificationService.show(NotificationType.info,
          `RBD creation in progress...`,
          this.taskManagerMessageService.getDescription(finishedTask));
        this.taskManagerService.subscribe(finishedTask.name, finishedTask.metadata,
          (asyncFinishedTask: FinishedTask) => {
            this.notificationService.notifyTask(asyncFinishedTask);
          });
      } else {
        finishedTask.success = true;
        this.notificationService.notifyTask(finishedTask);
      }
      this.router.navigate(['/block/rbd']);
    }, (resp) => {
      this.rbdForm.setErrors({'cdSubmitButton': true});
      finishedTask.success = false;
      finishedTask.exception = resp.error;
      this.notificationService.notifyTask(finishedTask);
    });
  }

  editRequest() {
    const request = new RbdFormEditRequestModel();
    request.name = this.rbdForm.get('name').value;
    request.size = this.formatter.toBytes(this.rbdForm.get('size').value);
    if (!this.defaultFeaturesFormControl.value) {
      _.forIn(this.features, (feature) => {
        if (this.featuresFormGroups.get(feature.key).value) {
          request.features.push(feature.key);
        }
      });
    }
    return request;
  }

  editAction() {
    const request = this.editRequest();
    const finishedTask = new FinishedTask();
    finishedTask.name = 'rbd/edit';
    finishedTask.metadata = {
      'pool_name':  this.response.pool_name,
      'image_name': this.response.name
    };
    this.rbdService.update(this.response.pool_name, this.response.name, request)
      .toPromise().then((resp) => {
        if (resp.status === 202) {
          this.notificationService.show(NotificationType.info,
            `RBD update in progress...`,
            this.taskManagerMessageService.getDescription(finishedTask));
          this.taskManagerService.subscribe(finishedTask.name, finishedTask.metadata,
            (asyncFinishedTask: FinishedTask) => {
              this.notificationService.notifyTask(asyncFinishedTask);
            });
        } else {
          finishedTask.success = true;
          this.notificationService.notifyTask(finishedTask);
        }
        this.router.navigate(['/block/rbd']);
      }).catch((resp) => {
        this.rbdForm.setErrors({'cdSubmitButton': true});
        finishedTask.success = false;
        finishedTask.exception = resp.error;
        this.notificationService.notifyTask(finishedTask);
      });
  }

  submit() {
    if (this.editing) {
      this.editAction();
    } else {
      this.createAction();
    }
  }

}
