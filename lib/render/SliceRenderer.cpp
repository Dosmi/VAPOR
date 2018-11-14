#include <sstream>
#include <string>

#include <vapor/SliceRenderer.h>
#include <vapor/SliceParams.h>
#include <vapor/ControlExecutive.h>
#include <vapor/LegacyGL.h>
#include <vapor/GLManager.h>
#include <vapor/GetAppPath.h>

#define X 0
#define Y 1 
#define Z 2
#define XY 0
#define XZ 1
#define YZ 2
#define NUMVERTICES 4

#define MAXTEXTURESIZE 8000

bool useEBO = false;

using namespace VAPoR;

static RendererRegistrar<SliceRenderer> registrar(
    SliceRenderer::GetClassType(), SliceParams::GetClassType()
);

SliceRenderer::SliceRenderer(
    const ParamsMgr* pm,
    string winName,
    string dataSetName,
    string instanceName,
    DataMgr* dataMgr
) : Renderer(
    pm, 
    winName, 
    dataSetName, 
    SliceParams::GetClassType(),
    SliceRenderer::GetClassType(), 
    instanceName, 
    dataMgr
) {
    _initialized   = false;
    _textureWidth  = 250;
    _textureHeight = 250;

    _VAO        = 0;
    _vertexVBO  = 0;
    _dataVBO    = 0;
    _EBO        = 0;

    SliceParams* p = dynamic_cast<SliceParams*>(GetActiveParams());
    assert(p);
    MapperFunction* tf = p->GetMapperFunc(_cacheParams.varName);

    _colorMapSize = tf->getNumEntries();
    _colorMap = new GLfloat[_colorMapSize * 4];
    for (int i=0; i<_colorMapSize; i++) {
        _colorMap[i*4+0] = (float) i / (float) (_colorMapSize-1);
        _colorMap[i*4+1] = (float) i / (float) (_colorMapSize-1);
        _colorMap[i*4+2] = (float) i / (float) (_colorMapSize-1);
        _colorMap[i*4+3] = 1.0;
    }

    // _dataValues consists of 1) a data value, and 2) a missing value flag
    // at every sampled point in the plane of the texture
    _dataValues = new float[_textureWidth * _textureHeight];// * 2];
    
    _vertexPositions.clear();

    _saveCacheParams();
    _initialized = true;
}

SliceRenderer::~SliceRenderer() {
    //glDeleteTextures(1, &_textureID);
    glDeleteTextures(1, &_colorMapTextureID);

    if (_colorMap) {
        delete [] _colorMap;
        _colorMap = nullptr;
    }
    if (_dataValues) {
        delete [] _dataValues;
        _dataValues = nullptr;
    }

    glDeleteVertexArrays(1, &_VAO);
    //glDeleteBuffers(1, &_dataVBO);
    glDeleteBuffers(1, &_vertexVBO);
    glDeleteBuffers(1, &_EBO);
}

int SliceRenderer::_initializeGL() {
    return 0;
}

int SliceRenderer::_saveCacheParams() {
    SliceParams* p = dynamic_cast<SliceParams*>(GetActiveParams());
    assert(p);

    _cacheParams.varName            = p->GetVariableName();
    _cacheParams.heightVarName      = p->GetHeightVariableName();
    _cacheParams.ts                 = p->GetCurrentTimestep();
    _cacheParams.refinementLevel    = p->GetRefinementLevel();
    _cacheParams.compressionLevel   = p->GetCompressionLevel();
    _cacheParams.textureSampleRate  = p->GetSampleRate();
    _cacheParams.orientation        = p->GetBox()->GetOrientation();

    _textureWidth  = _cacheParams.textureSampleRate;
    _textureHeight = _cacheParams.textureSampleRate;
    if (_textureWidth > MAXTEXTURESIZE)
        _textureWidth = MAXTEXTURESIZE;
    if (_textureHeight > MAXTEXTURESIZE)
        _textureHeight = MAXTEXTURESIZE;
 
    p->GetBox()->GetExtents(_cacheParams.boxMin, _cacheParams.boxMax);

    MapperFunction* tf = p->GetMapperFunc(_cacheParams.varName);
    tf->makeLut(_cacheParams.tf_lut);
    cout << "ToDo: eliminate either _colorMap or tf_lut in SliceRenderer" << endl;
    tf->makeLut(_colorMap);
    _cacheParams.tf_minMax = tf->getMinMaxMapValue();
    
    if (_dataValues) delete [] _dataValues;
    _dataValues = new float[_textureWidth * _textureHeight];

    int rc = _saveTextureData();
    if (rc<0) 
        SetErrMsg("Unable to acquire data for Slice texture");
    
    return rc;
}

void SliceRenderer::_getSampleCoordinates(
    std::vector<double> &coords, 
    int i, 
    int j
) const {
    int sampleRate = _cacheParams.textureSampleRate;
    double dx = (_cacheParams.boxMax[X]-_cacheParams.boxMin[X])/(1+sampleRate);
    double dy = (_cacheParams.boxMax[Y]-_cacheParams.boxMin[Y])/(1+sampleRate);
    double dz = (_cacheParams.boxMax[Z]-_cacheParams.boxMin[Z])/(1+sampleRate);

    if (_cacheParams.orientation == XY) {
        coords[X] = _cacheParams.boxMin[X] + dx*i + dx/2.f;
        coords[Y] = _cacheParams.boxMin[Y] + dy*j + dy/2.f;
        coords[Z] = _cacheParams.boxMin[Z];
    }
    else if (_cacheParams.orientation == XZ) {
        coords[X] = _cacheParams.boxMin[X] + dx*i + dx/2.f;
        coords[Y] = _cacheParams.boxMin[Y];
        coords[Z] = _cacheParams.boxMin[Z] + dz*j + dz/2.f;
    }
    else {  // Y corresponds to i, the faster axis; Z to j, the slower axis
        coords[Z] = _cacheParams.boxMin[Z] + dz*j + dz/2.f;
        coords[Y] = _cacheParams.boxMin[Y] + dy*i + dy/2.f;
        coords[X] = _cacheParams.boxMin[X];
    }
}

int SliceRenderer::_saveTextureData() {
    Grid* grid = NULL;
    int rc = DataMgrUtils::GetGrids(
        _dataMgr, 
        _cacheParams.ts,
        _cacheParams.varName,
        _cacheParams.boxMin,
        _cacheParams.boxMax,
        true,
        &_cacheParams.refinementLevel,
        &_cacheParams.compressionLevel,
        &grid
    );

    grid->SetInterpolationOrder(1);
    
    if (rc<0) {
        SetErrMsg("Unable to acquire Grid for Slice texture");
        return(rc);
    }
    assert(grid);

    _setVertexPositions();

    SliceParams* p = dynamic_cast<SliceParams*>(GetActiveParams());
    assert(p);
    MapperFunction* tf = p->GetMapperFunc(_cacheParams.varName);
    tf->makeLut(_colorMap);

    float varValue, minValue, maxValue, missingValue; 
    std::vector<double> coords(3, 0.0); 
    for (int j=0; j<_textureHeight; j++) {
        for (int i=0; i<_textureWidth; i++) {
            _getSampleCoordinates(coords, i, j);

            //int index = (j*_textureWidth + i)*2;
            int index = (j*_textureWidth + i);

            varValue = grid->GetValue(coords);
            missingValue = grid->GetMissingValue();
            if (varValue == missingValue) {
                // The second element of our shader indicates a missing value
                // if it's not equal to 0, so set it to 1.f here
                
                _dataValues[index]   = 1.f;
                //_dataValues[index+1] = 1.f;
                continue;
            }

            // The second element of our shader indicates a missing value
            // if it's not equal to 0, so set it to 0 here
            _dataValues[index]   = varValue;
            //_dataValues[index+1] = 0.0f;
        }
    }

    return rc;
}

bool SliceRenderer::_isCacheDirty() const
{
    SliceParams* p = dynamic_cast<SliceParams*>(GetActiveParams());
    assert(p);

    if (_cacheParams.varName          != p->GetVariableName())       return true;
    if (_cacheParams.heightVarName    != p->GetHeightVariableName()) return true;
    if (_cacheParams.ts               != p->GetCurrentTimestep())    return true;
    if (_cacheParams.refinementLevel  != p->GetRefinementLevel())    return true;
    if (_cacheParams.compressionLevel != p->GetCompressionLevel())   return true;

    if (_cacheParams.textureSampleRate != p->GetSampleRate())        return true;

    MapperFunction *tf = p->GetMapperFunc(_cacheParams.varName);
    vector <float> tf_lut;
    tf->makeLut(tf_lut);
    if (_cacheParams.tf_lut != tf_lut)                               return true;
    if (_cacheParams.tf_minMax != tf->getMinMaxMapValue())           return true;

    vector<double> min, max;
    Box* box = p->GetBox();
    box->GetExtents(min, max);
    int orientation = box->GetOrientation();

    if (_cacheParams.orientation      != orientation)                return true;
    if (_cacheParams.boxMin != min)                                  return true;
    if (_cacheParams.boxMax != max)                                  return true;

    return false;
}

int SliceRenderer::_paintGL(bool fast) {
    int rc = 0;
    if (_isCacheDirty()) {
        rc = _saveCacheParams();
    }

    _initializeState();

    glGenVertexArrays(1, &_VAO);
    glBindVertexArray(_VAO);

    glGenBuffers(1, &_vertexVBO);
    glBindBuffer(GL_ARRAY_BUFFER, _vertexVBO);
    glVertexAttribPointer(0, 3, GL_DOUBLE, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);
    if (useEBO) {
        glBufferData(
            GL_ARRAY_BUFFER, 
            NUMVERTICES*3*sizeof(double), 
            &_vertexPositions[0], 
            GL_STATIC_DRAW
        );
    }
    else {
        glBufferData(
            GL_ARRAY_BUFFER, 
            6*3*sizeof(double), 
            &_vertexPositions[0], 
            GL_STATIC_DRAW
        );
    }

    float texCoords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f
        /*1.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 1.0f*/
    };
    glGenBuffers(1, &_texCoordVBO);
    glBindBuffer(GL_ARRAY_BUFFER, _texCoordVBO);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(1);
    glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(texCoords),
        texCoords,
        GL_STATIC_DRAW
    );

/*    glGenBuffers(1, &_dataVBO);
    glBindBuffer(GL_ARRAY_BUFFER, _dataVBO);
    //glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(1);
    //size_t dataSize = _textureWidth*_textureHeight*2*sizeof(float);
    size_t dataSize = _textureWidth*_textureHeight*sizeof(float);
    glBufferData(
        GL_ARRAY_BUFFER, 
        dataSize,//sizeof(_dataValues), 
        _dataValues, 
        GL_STATIC_DRAW
    );
*/


    if (useEBO) {
        glGenBuffers(1, &_EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _EBO);
        unsigned short indexValues [] = { 0, 1, 2, 1, 3, 2  };
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER, 
            //indexValues.size()*sizeof(unsigned short), 
            sizeof(indexValues),
            //&indexValues[0], 
            &indexValues,
            GL_STATIC_DRAW
        );
    }

    _configureTextures();
    _configureShader();

    if (printOpenGLError() !=0) {
        cout << "A" << endl;
        return -1;
    }


// Question:
// the TwoDRenderer binds its colormap to two textures??? why???
//      glActiveTexture(GL_TEXTURE1);
//      glBindTexture(GL_TEXTURE_1D, _cMapTexID);
   

    glBindVertexArray(_VAO);
    if (useEBO) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _EBO);
        //glDrawElements(GL_TRIANGLES, 2, GL_UNSIGNED_INT, 0); 
        glDrawArrays(GL_TRIANGLES, 0, 4);
    }
    else {
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    _resetState();

    if (printOpenGLError() !=0) {
        cout << "B" << endl;
        return -1;
    }
    return rc;
}

void SliceRenderer::_configureShader() {
    ShaderProgram* s = _glManager->shaderManager->GetShader("Slice");
    EnableClipToBox(s);
    s->Bind();
    
    // One vertex shader uniform vec4
    s->SetUniform("MVP", _glManager->matrixManager->GetModelViewProjectionMatrix());

    // Remaining fragment shader uniform floats
    SliceParams* p = dynamic_cast<SliceParams*>(GetActiveParams());
    assert(p);
    float opacity = p->GetConstantOpacity();
    s->SetUniform("constantOpacity", opacity);
    s->SetUniform("minLUTValue", (float)_cacheParams.tf_minMax[0]);
    s->SetUniform("maxLUTValue", (float)_cacheParams.tf_minMax[1]);

    // And finally our uniform samplers
    GLint colormapLocation;
    colormapLocation = s->GetUniformLocation("colormap");
    glUniform1i(colormapLocation, 0);

    GLint dataValuesLocation;
    dataValuesLocation = s->GetUniformLocation("dataValues");
    glUniform1i(dataValuesLocation, 1);
}

void SliceRenderer::_configureTextures() {
    // Configure our colormap texture
    //
    glGenTextures(1, &_colorMapTextureID);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_1D, _colorMapTextureID);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage1D(
        GL_TEXTURE_1D, 
        0, 
        GL_RGBA8, 
        _colorMapSize,
        0, 
        GL_RGBA, 
        GL_FLOAT, 
        _colorMap
    );

    //glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &_dataValueTextureID);
    //glActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE1);
    //glActiveTexture(GL_TEXTURE0+1);
    glBindTexture(GL_TEXTURE_2D, _dataValueTextureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /*float dataValues[2][2]; 
    dataValues[0][0] = (float)0.f;
    dataValues[0][1] = (float)0.25f;
    dataValues[1][0] = (float)0.5f;
    dataValues[1][1] = (float)0.75f;*/

    glTexImage2D(
        GL_TEXTURE_2D, 
        0, 
        GL_R32F, 
        //2,
        //2,
        _textureWidth,
        _textureHeight,
        0, 
        GL_RED, 
        GL_FLOAT, 
        _dataValues
    );
}

void SliceRenderer::_initializeState() {
    _glManager->matrixManager->MatrixModeModelView();
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);
}

void SliceRenderer::_resetState() {
    // Unbind everything 
    //glDisableVertexAttribArray(0);
    //glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_1D, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Need to delete textures
}

void SliceRenderer::_render(
    int orientation,
    std::vector<double> min, 
    std::vector<double> max
) const {
    int plane;
    if (orientation == Z)
        plane = XY;
    else if (orientation == Y)
        plane = XZ;
    else
        plane = YZ;

    min[plane] = max[plane];

    LegacyGL *lgl = _glManager->legacy;

    lgl->Begin(GL_TRIANGLES);
    lgl->TexCoord2f(0.f, 0.f); lgl->Vertex3f(min[X], min[Y], min[Z]);
    lgl->TexCoord2f(1.f, 0.f); lgl->Vertex3f(max[X], min[Y], min[Z]);
    lgl->TexCoord2f(1.f, 1.f); lgl->Vertex3f(max[X], max[Y], max[Z]);

    lgl->TexCoord2f(0.f, 0.f); lgl->Vertex3f(min[X], min[Y], min[Z]);
    lgl->TexCoord2f(1.f, 1.f); lgl->Vertex3f(max[X], max[Y], max[Z]);
    lgl->TexCoord2f(0.f, 1.f); lgl->Vertex3f(min[X], max[Y], max[Z]);
    lgl->End();
}

void SliceRenderer::_setVertexPositions() {
    std::vector<double> min = _cacheParams.boxMin;
    std::vector<double> max = _cacheParams.boxMax;
    int orientation = _cacheParams.orientation;
    if (orientation == XY) {
        _setXYVertexPositions(min, max);
    }
    else if (orientation == XZ)
        _setXZVertexPositions(min, max);
    else if (orientation == YZ)
        _setXZVertexPositions(min, max);
}

void SliceRenderer::_setXYVertexPositions(
    std::vector<double> min, 
    std::vector<double> max
) {
    double zCoord = min[Z];

        cout << min[X] << " " <<  min[Y] << " " <<  zCoord << endl;
        cout << max[X] << " " <<  min[Y] << " " <<  zCoord << endl;
        cout << max[X] << " " <<  max[Y] << " " <<  zCoord << endl;
        cout << min[X] << " " <<  max[Y] << " " <<  zCoord << endl;

    std::vector<double> temp;
    if (useEBO) {
        std::vector<double> temp = {
            min[X], min[Y], zCoord,
            max[X], min[Y], zCoord,
            max[X], max[Y], zCoord,
            min[X], max[Y], zCoord
        };
        _vertexPositions = temp;
    }
    else {
        std::vector<double> temp = {
            min[X], min[Y], zCoord,
            max[X], min[Y], zCoord,
            min[X], max[Y], zCoord,
            max[X], min[Y], zCoord,
            max[X], max[Y], zCoord,
            min[X], max[Y], zCoord
        };
        _vertexPositions = temp;
    }
    /*std::vector<double> temp = {
        0., 0., 0.,
        1., 0., 0.,
        1., 1., 0.,
        0., 1., 0.
    };*/

}

void SliceRenderer::_setXZVertexPositions(
    std::vector<double> min, 
    std::vector<double> max
) {
    double yCoord = min[Y];

    std::vector<double> temp = {
        min[X], yCoord, min[Z],
        max[X], yCoord, min[Z],
        max[X], yCoord, max[Z],
        min[X], yCoord, max[Z]
    };

    _vertexPositions = temp;
}

void SliceRenderer::_setYZVertexPositions(
    std::vector<double> min, 
    std::vector<double> max
) {
    double xCoord = min[X];

    std::vector<double> temp = {
        xCoord, min[Y], min[Z],
        xCoord, max[Y], min[Z],
        xCoord, max[Y], max[Z],
        xCoord, min[Y], max[Z]
    };

    _vertexPositions = temp;
}
